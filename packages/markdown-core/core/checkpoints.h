#ifndef MARKDOWN_CORE_CHECKPOINTS_H
#define MARKDOWN_CORE_CHECKPOINTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "summed_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE CHECKPOINTS of a session (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.1 and 5.3): where a re-parse may restart, and where it may
 * rejoin the parse before it.
 *
 * A CHECKPOINT is the state the line machine was in at the start of a line
 * of the document where a block opened: the open spine as a chain of
 * frames, innermost first; the block below the spine that a later line may
 * still change (the open leaf, or the last child of the innermost container)
 * with the node flags it had at that line start; and where the input's
 * high-water mark stood before and after the line, relative to the line
 * start. Every line that opens a block in a state a restart can reopen has
 * one (blocks.c, the ledger), so a restart reopens the spine at the block
 * the edit is in, or at the line start before it.
 *
 * An ENTRY is a block a checkpoint names, as a spine container or as the
 * block below: the node, and the summaries of its children its finalize folds
 * (E4). A FRAME is one open container of a spine with the node flags and the
 * carried state it had at a line start. Frames are shared: a checkpoint whose
 * spine and state are those of the one before it uses the same chain.
 *
 * Checkpoints and entries are each a summed tree (summed_tree.h): a
 * checkpoint measures the distance from the previous checkpoint's line start
 * to its own, an entry the distance from the previous entry's block start to
 * its own, so both are found by offset and an edit shifts everything after it
 * by changing one measure. */

struct markdown_core_node;
typedef struct markdown_core_entry markdown_core_entry;
typedef struct markdown_core_frame markdown_core_frame;
typedef struct markdown_core_checkpoint markdown_core_checkpoint;

struct markdown_core_entry {
    markdown_core_summed_node link;
    /* The block, or NULL once it is released. */
    struct markdown_core_node *node;
    /* The frames and checkpoints that name the entry. */
    uint32_t holds;
    /* Whether the entry is in the tree: a parse places its new entries as it
     * commits. */
    bool placed;
    /* The fold of the block's children but its last, and its last child's
     * summary (E4). */
    uint32_t sum, last;
    /* The block's carried flags and word at the line start a parse's
     * ledger is building a checkpoint for, valid in the replay `epoch`
     * (blocks.c). */
    uint16_t bits;
    uint32_t epoch;
    uint64_t carry;
};

struct markdown_core_frame {
    markdown_core_frame *parent;
    markdown_core_entry *entry;
    uint32_t holds;
    /* The container's flags at the line start, but for being open, and the
     * state its element carries from line to line. */
    uint16_t bits;
    uint64_t carry;
};

enum markdown_core_checkpoint_marks {
    /* A block was below the spine: `below` names it. */
    MARKDOWN_CORE_CHECKPOINT_BELOW = 1 << 0,
    /* The block below was open, and the line closed it without asking it
     * whether it continues: it was settled before the line. */
    MARKDOWN_CORE_CHECKPOINT_LEAF = 1 << 1,
};

struct markdown_core_checkpoint {
    markdown_core_summed_node link;
    markdown_core_frame *frame;
    markdown_core_entry *below;
    uint16_t below_bits;
    uint8_t marks;
    /* The high-water mark before the line was read and after it was
     * processed, from the line start. */
    uint32_t frontier, after;
};

static inline markdown_core_checkpoint *markdown_core_checkpoint_of(markdown_core_summed_node *link) {
    return (markdown_core_checkpoint *)(void *)link;
}
static inline markdown_core_entry *markdown_core_entry_of(markdown_core_summed_node *link) {
    return (markdown_core_entry *)(void *)link;
}

typedef struct markdown_core_checkpoints {
    markdown_core_summed_tree lines, entries;
    /* THE TOUCHED SPAN of the document: the source range of every block a
     * registry access or a write to a closed block was made from (parser.h,
     * markdown_core_parser_touch). A re-parse that reads any of it reads all
     * of it. Empty when `touched_end` is 0. */
    size_t touched_start, touched_end;
    /* The last replay a parse's ledger made (blocks.c): an entry's carried
     * state is the replay's while its `epoch` is this. */
    uint32_t epoch;
    /* Storage: entries, frames and checkpoints come from chunks and go back
     * to free lists, so a pointer to any of them is stable for its life. */
    struct markdown_core_checkpoint_chunk *chunks;
    markdown_core_entry *free_entries;
    markdown_core_frame *free_frames;
    markdown_core_checkpoint *free_checkpoints;
} markdown_core_checkpoints;

/* An entry, a frame or a checkpoint from the store; NULL when it could not
 * allocate. A new frame holds its parent and its entry. */
markdown_core_entry *markdown_core_entry_new(markdown_core_checkpoints *store, struct markdown_core_node *node);
markdown_core_frame *markdown_core_frame_new(markdown_core_checkpoints *store, markdown_core_frame *parent,
                                             markdown_core_entry *entry, uint16_t bits, uint64_t carry);
markdown_core_checkpoint *markdown_core_checkpoint_new(markdown_core_checkpoints *store);

static inline void markdown_core_entry_hold(markdown_core_entry *entry) {
    if (entry) {
        entry->holds++;
    }
}
static inline void markdown_core_frame_hold(markdown_core_frame *frame) {
    if (frame) {
        frame->holds++;
    }
}
/* Drops one hold. An entry held by nothing leaves its tree and its node; a
 * frame held by nothing drops its entry and its parent. */
void markdown_core_entry_drop(markdown_core_checkpoints *store, markdown_core_entry *entry);
void markdown_core_frame_drop(markdown_core_checkpoints *store, markdown_core_frame *frame);
/* Gives a checkpoint back, dropping what it holds. It must be out of its
 * tree. */
void markdown_core_checkpoint_free(markdown_core_checkpoints *store, markdown_core_checkpoint *checkpoint);

/* Gives back every checkpoint and entry, leaving the store empty but for its
 * storage. */
void markdown_core_checkpoints_clear(markdown_core_checkpoints *store);
void markdown_core_checkpoints_dispose(markdown_core_checkpoints *store);

/* Where a checkpoint's line starts, and where an entry's block starts. */
static inline size_t markdown_core_checkpoint_line(const markdown_core_checkpoint *checkpoint) {
    return markdown_core_summed_before(&checkpoint->link) + checkpoint->link.own;
}
static inline size_t markdown_core_entry_start(const markdown_core_entry *entry) {
    return markdown_core_summed_before(&entry->link) + entry->link.own;
}

#ifdef __cplusplus
}
#endif

#endif
