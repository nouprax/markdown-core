#include <string.h>

#include "alloc.h"
#include "block_records.h"
#include "node.h"

/* A chunk of storage: records and frames of one session are carved from it
 * in turn and never move. */
#define RECORD_CHUNK_BYTES ((size_t)16384)

/* The unit storage is carved in, aligned for any record or frame. */
typedef union {
    void *pointer;
    uint64_t word;
    long double number;
} record_unit;

typedef struct markdown_core_block_record_chunk {
    struct markdown_core_block_record_chunk *next;
    size_t used;
    record_unit units[RECORD_CHUNK_BYTES / sizeof(record_unit)];
} markdown_core_block_record_chunk;

static void *carve(markdown_core_block_records *records, size_t size) {
    size = (size + sizeof(record_unit) - 1) / sizeof(record_unit);
    markdown_core_block_record_chunk *chunk = records->chunks;
    if (!chunk || chunk->used + size > sizeof(chunk->units) / sizeof(record_unit)) {
        chunk = markdown_core_alloc(1, sizeof(*chunk));
        if (!chunk) {
            return NULL;
        }
        chunk->next = records->chunks;
        records->chunks = chunk;
    }
    void *made = chunk->units + chunk->used;
    chunk->used += size;
    return made;
}

markdown_core_block_record *markdown_core_block_record_new(markdown_core_block_records *records) {
    markdown_core_block_record *record = records->free_records;
    if (record) {
        records->free_records = (markdown_core_block_record *)(void *)record->link.up;
    } else if (!(record = carve(records, sizeof(*record)))) {
        return NULL;
    }
    memset(record, 0, sizeof(*record));
    return record;
}

markdown_core_frame *markdown_core_frame_new(markdown_core_block_records *records, markdown_core_frame *parent,
                                             markdown_core_block_record *record, uint16_t bits) {
    markdown_core_frame *frame = records->free_frames;
    if (frame) {
        records->free_frames = frame->parent;
    } else if (!(frame = carve(records, sizeof(*frame)))) {
        return NULL;
    }
    *frame = (markdown_core_frame){parent, record, 0, bits};
    if (parent) {
        parent->holds++;
    }
    return frame;
}

void markdown_core_frame_hold(markdown_core_frame *frame) {
    if (frame) {
        frame->holds++;
    }
}

void markdown_core_frame_drop(markdown_core_block_records *records, markdown_core_frame *frame) {
    while (frame && !--frame->holds) {
        markdown_core_frame *parent = frame->parent;
        frame->parent = records->free_frames;
        records->free_frames = frame;
        frame = parent;
    }
}

void markdown_core_block_record_free(markdown_core_block_records *records, markdown_core_block_record *record) {
    if (record->node) {
        record->node->record = NULL;
    }
    markdown_core_frame_drop(records, record->frame);
    record->frame = NULL;
    record->link.up = (markdown_core_summed_node *)(void *)records->free_records;
    records->free_records = record;
}

void markdown_core_block_records_clear(markdown_core_block_records *records) {
    /* Records are freed as the in-order walk leaves them; a balanced tree is
     * under 64 levels high. */
    markdown_core_summed_node *stack[64];
    size_t depth = 0;
    markdown_core_summed_node *link = records->tree.root;
    while (link || depth) {
        while (link) {
            stack[depth++] = link;
            link = link->before;
        }
        link = stack[--depth];
        markdown_core_summed_node *after = link->after;
        markdown_core_block_record_free(records, markdown_core_block_record_of(link));
        link = after;
    }
    records->tree.root = NULL;
}

void markdown_core_block_records_dispose(markdown_core_block_records *records) {
    markdown_core_block_records_clear(records);
    while (records->chunks) {
        markdown_core_block_record_chunk *chunk = records->chunks;
        records->chunks = chunk->next;
        markdown_core_free(chunk);
    }
    memset(records, 0, sizeof(*records));
}
