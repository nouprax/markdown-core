#include "concrete_records.h"

#include <assert.h>

#include "node.h"

// One allocation per vector: header and records travel together, so growth
// is a single realloc and ownership is a single pointer on the node. The
// vector starts empty and small — most regions hold a handful of markers —
// and doubles, so a region's capture stays linear in its own record count.

#define CONCRETE_RECORDS_FIRST_CAPACITY 4

bool markdown_core_concrete_records_append(
    markdown_core_mem *mem,
    markdown_core_concrete_records **slot,
    uint8_t kind,
    uint32_t line,
    uint32_t column,
    uint32_t length
) {
    markdown_core_concrete_records *records = *slot;
    markdown_core_concrete_record *record;

    if (!records || records->count == records->capacity) {
        size_t capacity;
        /* Growth ceiling: doubling a vector this large would wrap the byte
         * request and hand a huge capacity a tiny allocation. Parsing
         * cannot reach this on a 64-bit target — memory exhausts first —
         * but the failure contract must not lean on address-space size, so
         * the refusal is checked, not argued, and the gate drives this arm
         * directly. */
        if (records && records->capacity > (SIZE_MAX - sizeof(markdown_core_concrete_records)) /
                                               sizeof(markdown_core_concrete_record) / 2) {
            return false;
        }
        capacity = records ? records->capacity * 2 : CONCRETE_RECORDS_FIRST_CAPACITY;
        markdown_core_concrete_records *grown = (markdown_core_concrete_records *)mem->realloc(
            mem,
            records,
            sizeof(markdown_core_concrete_records) + capacity * sizeof(markdown_core_concrete_record)
        );
        if (!grown) {
            /* The old vector, when there is one, is still intact and still
             * owned by the node; the caller decides what a lost record
             * means. */
            return false;
        }
        if (!records) {
            /* A grown block keeps its counters; a fresh one starts empty
             * (the allocator does not zero realloc'd memory). */
            grown->count = 0;
        }
        grown->capacity = capacity;
        records = grown;
        *slot = records;
    }

    record = &records->records[records->count++];
    record->line = line;
    record->column = column;
    record->length = length;
    record->kind = kind;
    record->flags = 0;
    return true;
}

void markdown_core_concrete_records_free(markdown_core_mem *mem, markdown_core_concrete_records *records) {
    if (records) {
        mem->free(mem, records);
    }
}

const markdown_core_concrete_record *markdown_core_node_concrete_records(
    const markdown_core_node *node,
    size_t *count
) {
    if (!node->concrete) {
        *count = 0;
        return NULL;
    }
    *count = node->concrete->count;
    return node->concrete->records;
}

// --- inline capture -----------------------------------------------------------

// The building-phase tombstone: set when a reduce reinterprets already
// captured material as raw source, stripped by the compaction in take().
// Records handed to a node never carry it, which is what lets `flags` stay
// the reserved 14.1.10 byte the header promises.
#define INLINE_CONCRETE_TOMBSTONE 0x80u

void markdown_core_concrete_capture_init(markdown_core_concrete_capture *capture, markdown_core_mem *mem) {
    capture->mem = mem;
    capture->records = NULL;
    capture->tombstones = 0;
}

bool markdown_core_concrete_capture_append(
    markdown_core_concrete_capture *capture,
    uint8_t kind,
    uint32_t start,
    uint32_t length,
    uint32_t head,
    uint32_t tail
) {
    markdown_core_inline_concrete_records *records = capture->records;
    markdown_core_inline_concrete_record *record;

    /* Capture order is source order — every site appends at or past the
     * subject's cursor, and the retraction paths only re-append above the
     * last survivor — but that is the capture sites' invariant, pinned by
     * the runner's ascending-vector walk rather than asserted here, where
     * the growth-ceiling gate fabricates a capacity no real vector backs. */
    assert(capture->mem);
    assert(length > 0);
    assert(head <= length && tail <= length - head);

    if (!records || records->count == records->capacity) {
        size_t capacity;
        /* The same growth ceiling as the block append: doubling past this
         * point would wrap the byte request, and the refusal must not lean
         * on the address space making it unreachable. */
        if (records && records->capacity > (SIZE_MAX - sizeof(markdown_core_inline_concrete_records)) /
                                               sizeof(markdown_core_inline_concrete_record) / 2) {
            return false;
        }
        capacity = records ? records->capacity * 2 : CONCRETE_RECORDS_FIRST_CAPACITY;
        markdown_core_inline_concrete_records *grown = (markdown_core_inline_concrete_records *)capture->mem->realloc(
            capture->mem,
            records,
            sizeof(markdown_core_inline_concrete_records) + capacity * sizeof(markdown_core_inline_concrete_record)
        );
        if (!grown) {
            return false;
        }
        if (!records) {
            grown->count = 0;
        }
        grown->capacity = capacity;
        records = grown;
        capture->records = records;
    }

    record = &records->records[records->count++];
    record->start = start;
    record->length = length;
    record->head = head;
    record->tail = tail;
    record->kind = kind;
    record->flags = 0;
    return true;
}

size_t markdown_core_concrete_capture_count(const markdown_core_concrete_capture *capture) {
    return capture->records ? capture->records->count : 0;
}

/* Patch targets are records this parse appended; the append-only vector
 * keeps every index stable until take(). */
static markdown_core_inline_concrete_record *capture_record_at(
    markdown_core_concrete_capture *capture,
    size_t index
) {
    assert(capture->records && index < capture->records->count);
    return &capture->records->records[index];
}

void markdown_core_concrete_capture_consume(
    markdown_core_concrete_capture *capture,
    size_t index,
    uint32_t head,
    uint32_t tail
) {
    markdown_core_inline_concrete_record *record = capture_record_at(capture, index);
    record->head += head;
    record->tail += tail;
    assert(record->head <= record->length && record->tail <= record->length - record->head);
}

void markdown_core_concrete_capture_consume_all(markdown_core_concrete_capture *capture, size_t index) {
    markdown_core_inline_concrete_record *record = capture_record_at(capture, index);
    record->head = record->length;
    record->tail = 0;
}

void markdown_core_concrete_capture_set_kind(markdown_core_concrete_capture *capture, size_t index, uint8_t kind) {
    capture_record_at(capture, index)->kind = kind;
}

void markdown_core_concrete_capture_tombstone_from(markdown_core_concrete_capture *capture, size_t index) {
    size_t i;
    if (!capture->records) {
        return;
    }
    for (i = index; i < capture->records->count; i++) {
        markdown_core_inline_concrete_record *record = &capture->records->records[i];
        if (!(record->flags & INLINE_CONCRETE_TOMBSTONE)) {
            record->flags |= INLINE_CONCRETE_TOMBSTONE;
            capture->tombstones++;
        }
    }
}

void markdown_core_concrete_capture_tombstone_span(
    markdown_core_concrete_capture *capture,
    uint32_t start,
    uint32_t end
) {
    size_t i;
    if (!capture->records) {
        return;
    }
    /* Records ascend by start, so the walk backs off the tail; the span's
     * own endpoints (the reducer's delimiter records) lie outside [start,
     * end) and stay. */
    for (i = capture->records->count; i > 0; i--) {
        markdown_core_inline_concrete_record *record = &capture->records->records[i - 1];
        if (record->start + record->length > end) {
            continue;
        }
        if (record->start < start) {
            break;
        }
        if (!(record->flags & INLINE_CONCRETE_TOMBSTONE)) {
            record->flags |= INLINE_CONCRETE_TOMBSTONE;
            capture->tombstones++;
        }
    }
}

markdown_core_inline_concrete_records *markdown_core_concrete_capture_take(markdown_core_concrete_capture *capture) {
    markdown_core_inline_concrete_records *records = capture->records;
    markdown_core_mem *mem = capture->mem;
    size_t read;
    size_t write = 0;

    capture->records = NULL;
    if (!records) {
        capture->tombstones = 0;
        return NULL;
    }
    if (capture->tombstones) {
        for (read = 0; read < records->count; read++) {
            if (records->records[read].flags & INLINE_CONCRETE_TOMBSTONE) {
                continue;
            }
            records->records[write++] = records->records[read];
        }
        records->count = write;
        capture->tombstones = 0;
    }
    if (records->count == 0) {
        mem->free(mem, records);
        return NULL;
    }
    return records;
}

void markdown_core_concrete_capture_abandon(markdown_core_concrete_capture *capture) {
    if (capture->records) {
        capture->mem->free(capture->mem, capture->records);
        capture->records = NULL;
    }
    capture->tombstones = 0;
}

void markdown_core_inline_concrete_records_free(
    markdown_core_mem *mem,
    markdown_core_inline_concrete_records *records
) {
    if (records) {
        mem->free(mem, records);
    }
}

const markdown_core_inline_concrete_record *markdown_core_node_inline_concrete_records(
    const markdown_core_node *node,
    size_t *count
) {
    if (!node->inline_concrete) {
        *count = 0;
        return NULL;
    }
    *count = node->inline_concrete->count;
    return node->inline_concrete->records;
}
