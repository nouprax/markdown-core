#ifndef MARKDOWN_CORE_BLOCK_RECORDS_H
#define MARKDOWN_CORE_BLOCK_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "summed_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE BLOCK RECORDS of a session (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.1 and 5.3: the block ledger). One record per block the block
 * parser opened in the document's own input, in the order they were opened,
 * in a summed tree (summed_tree.h) whose measure is the distance from the
 * previous record's line start to the record's own: the start of the line a
 * block opened on is a prefix sum, and an edit shifts every later record by
 * changing one measure.
 *
 * The FIRST record of each line is a CHECKPOINT: the state the line machine
 * was in at the start of that line, from which a re-parse can run on. It
 * holds the open spine at the line start as a chain of frames, innermost
 * first; the block below the spine that a later line may still change (the
 * open leaf, or the last child of the innermost container); and where the
 * input's high-water mark stood before and after the line, relative to the
 * line start.
 *
 * A FRAME is one open container of a spine with the node flags it carried at
 * a line start. Frames are shared: a line whose spine and flags are those of
 * the line before uses the same chain, so their storage is one frame per
 * container plus one per change of its flags. */

struct markdown_core_node;
typedef struct markdown_core_frame markdown_core_frame;
typedef struct markdown_core_block_record markdown_core_block_record;

struct markdown_core_frame {
    markdown_core_frame *parent;
    markdown_core_block_record *record;
    uint32_t holds;
    /* The container's flags at the line start, but for being open. */
    uint16_t bits;
};

enum markdown_core_block_record_marks {
    /* The first record of its line: the checkpoint fields hold. */
    MARKDOWN_CORE_BLOCK_RECORD_CHECKPOINT = 1 << 0,
    /* `below` was open at the line start: the line's leaf. */
    MARKDOWN_CORE_BLOCK_RECORD_LEAF = 1 << 1,
    /* The line closed the open leaf without asking it whether it continues:
     * the leaf was SETTLED before the line, and a restart there may reopen it
     * as it is. */
    MARKDOWN_CORE_BLOCK_RECORD_SETTLED = 1 << 2,
    /* The open leaf would take a lazy line. */
    MARKDOWN_CORE_BLOCK_RECORD_LAZY = 1 << 3,
    /* The line read or wrote a block that had closed before it. */
    MARKDOWN_CORE_BLOCK_RECORD_REACH = 1 << 4,
    /* A later line wrote into this record's block after it closed. */
    MARKDOWN_CORE_BLOCK_RECORD_WRITTEN = 1 << 5,
    /* A block was below the spine at the line start: `below` is its record,
     * NULL when it has none. */
    MARKDOWN_CORE_BLOCK_RECORD_BELOW = 1 << 6,
};

struct markdown_core_block_record {
    markdown_core_summed_node link;
    /* The block, or NULL once it is released. */
    struct markdown_core_node *node;
    /* The block's start, from the start of its line. */
    int32_t column;
    uint8_t marks;
    /* Checkpoint fields. */
    uint16_t below_bits;
    uint32_t frontier, after;
    markdown_core_frame *frame;
    markdown_core_block_record *below;
};

/* The records' measure: line starts. */
#define MARKDOWN_CORE_BLOCK_RECORD_LINE 0

static inline markdown_core_block_record *markdown_core_block_record_of(markdown_core_summed_node *link) {
    return (markdown_core_block_record *)(void *)link;
}

typedef struct markdown_core_block_records {
    markdown_core_summed_tree tree;
    /* Storage: records and frames come from chunks and go back to free
     * lists, so a pointer to either is stable for its life. */
    struct markdown_core_block_record_chunk *chunks;
    markdown_core_block_record *free_records;
    markdown_core_frame *free_frames;
} markdown_core_block_records;

/* A record or a frame from the store; NULL when it could not allocate. */
markdown_core_block_record *markdown_core_block_record_new(markdown_core_block_records *records);
markdown_core_frame *markdown_core_frame_new(markdown_core_block_records *records, markdown_core_frame *parent,
                                             markdown_core_block_record *record, uint16_t bits);
void markdown_core_frame_hold(markdown_core_frame *frame);
/* Drops one hold, releasing the frame and then each parent it held last. */
void markdown_core_frame_drop(markdown_core_block_records *records, markdown_core_frame *frame);
/* Gives a record back, dropping its frame and parting it from its node. The
 * record must be out of the tree. */
void markdown_core_block_record_free(markdown_core_block_records *records, markdown_core_block_record *record);
/* Gives back every record in the tree, leaving it empty: the nodes they
 * recorded keep none. */
void markdown_core_block_records_clear(markdown_core_block_records *records);
void markdown_core_block_records_dispose(markdown_core_block_records *records);

/* Where the line a record opened on starts. */
static inline size_t markdown_core_block_record_line(const markdown_core_block_record *record) {
    return markdown_core_summed_before(&record->link, MARKDOWN_CORE_BLOCK_RECORD_LINE) +
           record->link.own[MARKDOWN_CORE_BLOCK_RECORD_LINE];
}

#ifdef __cplusplus
}
#endif

#endif
