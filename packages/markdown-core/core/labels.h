#ifndef MARKDOWN_CORE_LABELS_H
#define MARKDOWN_CORE_LABELS_H

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

/* THE LABELS: what an editor's jump to definition and find references
 * read, kept as the parse reads the text and as an edit reads it again.
 *
 * A LABEL is a space and a name in that space's normal form. The spaces
 * are lookup spaces: the engine's own,
 *
 *   ANCHOR     an explicit anchor, reserved where completion meets it, as
 *              its bytes as written;
 *
 * and those the dialect's elements declare (element.h, markdown_core_space),
 * numbered after it as the dialect is sealed: a link reference
 * definition's labels, say, which a heading's text defines too. A space
 * that `folds` holds its names in the reference-label normal form
 * (markdown_core_labels_normal_label), any other its bytes as written. A
 * label's key is its space's byte, its name and NUL.
 *
 * A MENTION is a definition or a reference of a label, at a location in the
 * text, and is a node's, its OWNER, which takes it when it goes. Both ways
 * are indexed:
 *
 *   NAME TO MENTIONS   a label holds its definitions in the order of their
 *                      locations, the first of which a lookup answers with
 *                      (jump to definition), and its references (find
 *                      references);
 *   NODE TO MENTIONS   a node holds the mentions it owns (node.h,
 *                      `mentions`), which go with it
 *                      (markdown_core_labels_release).
 *
 * A definition is a node's (`node`, or none for an anchor, whose owner is
 * the node it names) and is located where its node starts. A definition of
 * a node may name nothing, which locates the node and defines nothing: the
 * document lists it all the same. A node defines in a sequence once: a
 * definition again replaces its earlier one there.
 *
 * A reference is a lookup an inline root's completion made (`node`, the
 * root), located at the byte of the text the lookup reads (blocks.c, WHERE
 * A USE IS), whatever it found, and keeps whether it found a definition
 * (`found`). Its owner is the root until the root's completion ends, and
 * then the child of the root whose text holds it (markdown_core_labels_own).
 *
 * THE SEQUENCES. The mentions are in sequences, each in the order of their
 * locations, in a tree (tree.h): the definitions of a space that nodes of a
 * kind make, one sequence for each (space, node kind), or the space alone
 * for definitions of no node; and the references, in one. So a document
 * lists its footnotes, its link reference definitions or its headings that
 * define a label each from a sequence (ast.c, A DOCUMENT'S TABLES). Each
 * tree's focus is an edit's: the mentions before where the reading starts
 * in the front, the rest in the back, which the reading does not see, until
 * it takes the old text back from a place (THE SPLICE, blocks.c), the
 * back's mentions before that place going with the text they were in, with
 * their owners. Every sequence is cut at the same place, so two mentions of
 * two sequences are in order as their parts and locations are.
 *
 * THE NAMES IN ORDER: the labels that have a definition, in the order of
 * their keys (map.h), a tree a document lists a space's names from.
 *
 * THE ALTERED LABELS. A transaction (a parse, or an edit's readings, up to its
 * close) lists each label whose definitions it changed, once, with whether
 * it had a definition as the transaction began (`was`): the names (names.c)
 * read the anchors it changed, and an edit rereads the roots of the
 * references whose answer is not their label's any more
 * (markdown_core_parser_altered_uses). The next transaction's first change
 * starts the list empty, and a label that holds no mention goes then; one
 * that holds none and is not listed goes as its last mention does. */

/* A space: the engine's ANCHOR, or a number the dialect gave a space its
 * elements declare (dialect.h, `spaces`); a key's first byte. */
typedef unsigned markdown_core_label_space;
#define MARKDOWN_CORE_LABEL_ANCHOR 1u

struct markdown_core_mention;

/* A LABEL. */
typedef struct markdown_core_label {
    /* Its definitions, in the order of their locations, linked through
     * their `before` and `after`, the first holding the last in `before`,
     * or NULL while it has none; how many; and the definition a search of
     * the list begins at, the one it last took or one beside the last it
     * let go (S_link). */
    struct markdown_core_mention *first, *near;
    uint32_t count;
    /* Its references, in no order, linked through their `after`. */
    struct markdown_core_mention *uses;
    /* Whether THE ALTERED LABELS list it, and whether it had a definition as the
     * transaction began. */
    bool listed, was;
    bufsize_t key_len;
    unsigned char key[];
} markdown_core_label;

/* A MENTION. */
typedef struct markdown_core_mention {
    /* Its label's definitions or references before and after it. */
    struct markdown_core_mention *before, *after;
    /* Its owner, and the owner's next mention (node.h, `mentions`). */
    markdown_core_node *owner;
    struct markdown_core_mention *owned;
    /* Its label, or NULL for a definition that names nothing. */
    markdown_core_label *label;
    /* The node that defined it, or NULL for an anchor; of a reference, the
     * inline root whose completion looked it up. */
    const markdown_core_node *node;
    /* Where its location is in its sequence's tree (tree.h, A HANDLE). */
    markdown_core_tree_handle at;
    /* Its sequence. */
    uint8_t sequence;
    /* A reference's answer: whether its lookup found a definition; and
     * whether it still owes its owner, its root's completion not ended. */
    bool found, owed;
} markdown_core_mention;

/* A mention's place in its sequence: its location, and the mention. */
typedef struct {
    uint32_t start;
    markdown_core_mention *mention;
} markdown_core_mention_place;

/* A SEQUENCE (THE SEQUENCES). */
typedef struct markdown_core_sequence {
    markdown_core_tree places;
    markdown_core_label_space space;
    /* The kind of node that defines in it, or MARKDOWN_CORE_NODE_NONE, the
     * references' with space 0. */
    uint16_t node;
} markdown_core_sequence;

#define MARKDOWN_CORE_SEQUENCES 255

typedef struct markdown_core_labels {
    /* The sequences, each in storage of its own, as they are first defined
     * in; the references', or -1 before the first reference. */
    markdown_core_sequence **sequences;
    size_t sequence_count;
    int uses;
    /* How many mentions there are. */
    size_t count;
    /* Each key's label, in `value`; the index borrows the key from it. */
    markdown_core_key_index index;
    /* THE NAMES IN ORDER: a key entry (map.h) of each defined label, its
     * key borrowed from it and the label its `value`. */
    markdown_core_tree names;
    /* Where a lookup assembles its key. */
    markdown_core_strbuf key;
    /* THE ALTERED LABELS, in the order the transaction first changed them, and
     * whether the transaction closed, so that the next change starts the
     * next. */
    MARKDOWN_CORE_ARRAY(markdown_core_label *) altered;
    bool closed;
    /* The nodes a walk of the text that left has still to visit (blocks.c,
     * THE TEXT THAT LEFT). */
    MARKDOWN_CORE_ARRAY(struct markdown_core_node *) left;
} markdown_core_labels;

void markdown_core_labels_init(markdown_core_labels *labels);
/* Every label and mention let go: a node still alive that owned one is let
 * go with no word to the table (node.h, `mentions`). */
void markdown_core_labels_free(markdown_core_labels *labels);

/* `node`, or no node when it is NULL, defines `name` of `space`, already in
 * the space's normal form, located at `start`, and `owner` owns it,
 * replacing its earlier definition in the sequence. An empty name defines
 * nothing: of a node, the node is located all the same. False when storage
 * could not be had. */
bool markdown_core_labels_define(markdown_core_labels *labels, markdown_core_label_space space,
                                 const unsigned char *name, bufsize_t length, uint32_t start,
                                 const markdown_core_node *node, markdown_core_node *owner);

/* The inline root `root` refers to `name` of `space`, as written, at
 * `start`, and owns the reference until its completion ends. The answer is
 * the label's first definition, or NULL. A name of a space that `folds` is
 * looked up in its reference-label normal form, and one that normalizes to
 * nothing, or longer than a link label may be, is defined by nothing and
 * makes no reference, as an empty name of any space. `*failed` is set when
 * storage could not be had, and the answer is then NULL. */
const markdown_core_mention *markdown_core_labels_refer(markdown_core_labels *labels, markdown_core_label_space space,
                                                        const markdown_core_chunk *name, bool folds, uint32_t start,
                                                        markdown_core_node *root, bool *failed);

/* The reference `mention`, which its root owns and owes, is owned by
 * `owner`, the root or a child of it, and owes no more. */
void markdown_core_labels_own(markdown_core_mention *mention, markdown_core_node *owner);

/* `node`, which the parse lets go, takes its mentions with it. */
void markdown_core_labels_release(markdown_core_labels *labels, markdown_core_node *node);

/* The first definition of `space` for `name`, in the space's normal form,
 * or NULL. `*failed` is set when the key could not be assembled. */
const markdown_core_mention *markdown_core_labels_find(markdown_core_labels *labels, markdown_core_label_space space,
                                                       const unsigned char *name, bufsize_t length, bool *failed);

/* The same of an ANCHOR, `name` kept in pieces and measuring `measure`. */
const markdown_core_mention *markdown_core_labels_find_name(const markdown_core_labels *labels,
                                                            const markdown_core_pieces *name,
                                                            markdown_core_measure measure);

/* The first definition of `space` for the reference-label normal form of
 * `name`, as written; a name that normalizes to nothing, or is longer than
 * a link label may be, is defined by nothing. `*failed` as above. */
const markdown_core_mention *markdown_core_labels_find_label(markdown_core_labels *labels,
                                                             markdown_core_label_space space,
                                                             const markdown_core_chunk *name, bool *failed);

/* The first definition of `space` for `name`, in the space's normal form,
 * or NULL, with no change to anything: the next is its `after`. */
const markdown_core_mention *markdown_core_labels_seek(const markdown_core_labels *labels,
                                                       markdown_core_label_space space, const unsigned char *name,
                                                       bufsize_t length);

/* The sequence of `space` that nodes of kind `node` define in, or NULL when
 * none has. */
const markdown_core_sequence *markdown_core_labels_sequence(const markdown_core_labels *labels,
                                                            markdown_core_label_space space, uint16_t node);

/* The mention at rank `index` of the sequence. */
static inline const markdown_core_mention *markdown_core_sequence_at(const markdown_core_sequence *sequence,
                                                                     size_t index) {
    return ((const markdown_core_mention_place *)markdown_core_tree_peek(&sequence->places, index))->mention;
}

/* `name`'s reference-label normal form, in storage from `pool` the node
 * that holds it owns (markdown_core_node_pool_bytes), NUL-terminated; empty
 * when it normalizes to nothing. False when storage could not be had. */
bool markdown_core_labels_normal_label(markdown_core_labels *labels, markdown_core_node_pool *pool,
                                       const markdown_core_chunk *name, markdown_core_chunk *normal);

/* THE NAMES IN ORDER: those of `space`, `*count` of them, from the rank it
 * returns; and the label at a rank. */
size_t markdown_core_labels_names_of(const markdown_core_labels *labels, markdown_core_label_space space,
                                     size_t *count);
static inline const markdown_core_label *markdown_core_labels_name_at(const markdown_core_labels *labels, size_t rank) {
    return (const markdown_core_label *)((const markdown_core_key_entry *)markdown_core_tree_peek(&labels->names, rank))
        ->value;
}

/* A label's name, borrowed from it. */
static inline markdown_core_chunk markdown_core_label_name(const markdown_core_label *label) {
    markdown_core_chunk text = {(unsigned char *)label->key + 1, label->key_len - 1, 0};
    return text;
}

/* The name a mention is of: empty for a definition that names nothing. */
static inline markdown_core_chunk markdown_core_mention_name(const markdown_core_mention *mention) {
    return mention->label ? markdown_core_label_name(mention->label) : markdown_core_chunk_literal("");
}

/* Where a mention is located, in the terms of the part of its sequence that
 * holds it, and whether that is the back: the text before the edit, for
 * one of the back before the splice. */
uint32_t markdown_core_labels_located(const markdown_core_labels *labels, const markdown_core_mention *mention,
                                      bool *back);

static inline uint32_t markdown_core_labels_start(const markdown_core_labels *labels,
                                                  const markdown_core_mention *mention) {
    bool back;
    return markdown_core_labels_located(labels, mention, &back);
}

/* THE ALTERED LABELS: the labels, `*count` of them. */
static inline markdown_core_label *const *markdown_core_labels_altered(const markdown_core_labels *labels,
                                                                       size_t *count) {
    *count = labels->altered.count;
    return labels->altered.values;
}

/* The transaction is over: the next change starts the next. */
static inline void markdown_core_labels_close(markdown_core_labels *labels) { labels->closed = true; }

/* AN EDIT READS FROM `start` (blocks.c, THE REOPEN): the mentions located
 * there or past it, of every sequence, go to the back, and a transaction
 * begins. */
void markdown_core_labels_reopen(markdown_core_labels *labels, uint32_t start);

/* THE SPLICE (blocks.c): the back's locations move by `delta`. */
void markdown_core_labels_shift(markdown_core_labels *labels, int64_t delta);

#ifdef __cplusplus
}
#endif

#endif
