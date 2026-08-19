#include "concrete_records.h"

#include <stdlib.h>

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
    capture->retractions = NULL;
    capture->retraction_count = 0;
    capture->retraction_capacity = 0;
    capture->dirty = false;
}

void markdown_core_concrete_capture_adopt(
    markdown_core_concrete_capture *capture,
    markdown_core_inline_concrete_records *records
) {
    if (capture->mem) {
        capture->records = records;
    }
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

    /* Invariants argued, not branched on: the capture is engaged (every
     * caller runs under a parser-backed subject), tokens are never empty,
     * and head + tail never exceeds length — the runner's whole-tree
     * invariant walk pins all three on every parse. Capture order is
     * source order; the retraction paths only re-append above the last
     * survivor. */
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
/* Patch keys come from this parse's own appends (the engine's
 * capture_index, a bracket's floor), so they are always in range. */
static markdown_core_inline_concrete_record *capture_record_at(markdown_core_concrete_capture *capture, size_t index) {
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
    /* The caller retracts from a record it appended itself (the bracket's
     * own candidate), so the vector exists and `index` is in range; the
     * walk covers the bracket's records only, because nothing later has
     * been appended yet. Setting the bit is idempotent, so overlapping
     * retractions need no membership test — `dirty` just means handoff
     * must compact. */
    for (i = index; i < capture->records->count; i++) {
        capture->records->records[i].flags |= INLINE_CONCRETE_TOMBSTONE;
    }
    capture->dirty = true;
}

bool markdown_core_concrete_capture_retract_span(
    markdown_core_concrete_capture *capture,
    uint32_t start,
    uint32_t end
) {
    markdown_core_concrete_retraction *span;
    if (capture->retraction_count == capture->retraction_capacity) {
        size_t capacity;
        markdown_core_concrete_retraction *grown;
        /* The same wrap-point refusal as the record vectors. Successful
         * RANGE reduces retire at least two delimiter records each, so
         * spans stay below half the record count and hit this ceiling no
         * later than the records do — but the refusal must still not lean
         * on that arithmetic holding forever. */
        if (capture->retraction_capacity > SIZE_MAX / sizeof(*capture->retractions) / 2) {
            return false;
        }
        capacity = capture->retraction_capacity ? capture->retraction_capacity * 2 : CONCRETE_RECORDS_FIRST_CAPACITY;
        grown = (markdown_core_concrete_retraction *)
                    capture->mem->realloc(capture->mem, capture->retractions, capacity * sizeof(*capture->retractions));
        if (!grown) {
            return false;
        }
        capture->retractions = grown;
        capture->retraction_capacity = capacity;
    }
    span = &capture->retractions[capture->retraction_count++];
    span->start = start;
    span->end = end;
    return true;
}

static int retraction_start_compare(const void *left, const void *right) {
    const markdown_core_concrete_retraction *a = (const markdown_core_concrete_retraction *)left;
    const markdown_core_concrete_retraction *b = (const markdown_core_concrete_retraction *)right;
    return (int)(a->start > b->start) - (int)(a->start < b->start);
}

/* Applies the deferred retractions in one sweep: sort the spans, keep the
 * outermost of each nest, then walk records and spans together — both
 * ascend by start, so the cursor never backs up. Two spans are always
 * nested or disjoint, never crossing: constructs form a tree, and a
 * reduce that spans a rival candidate removes it from the engine before
 * it can ever reduce and note a crossing span of its own. Sorted by
 * start, a nested (inner) span therefore follows its outer one and ends
 * inside it, so "starts before the current span ends" means contained. */
static void capture_apply_retractions(markdown_core_concrete_capture *capture) {
    size_t merged = 0;
    size_t cursor = 0;
    size_t i;

    qsort(capture->retractions, capture->retraction_count, sizeof(*capture->retractions), retraction_start_compare);
    for (i = 1; i < capture->retraction_count; i++) {
        markdown_core_concrete_retraction *span = &capture->retractions[i];
        if (span->start >= capture->retractions[merged].end) {
            capture->retractions[++merged] = *span;
        }
    }
    merged++;

    for (i = 0; i < capture->records->count; i++) {
        markdown_core_inline_concrete_record *record = &capture->records->records[i];
        while (cursor < merged && capture->retractions[cursor].end <= record->start) {
            cursor++;
        }
        /* A record starting inside the span lies wholly inside it: records
         * never overlap, and the reducer's own closer record starts exactly
         * at the span's end, walling the interior off. */
        if (cursor < merged && record->start >= capture->retractions[cursor].start) {
            record->flags |= INLINE_CONCRETE_TOMBSTONE;
            capture->dirty = true;
        }
    }
}

markdown_core_inline_concrete_records *markdown_core_concrete_capture_take(markdown_core_concrete_capture *capture) {
    markdown_core_inline_concrete_records *records = capture->records;
    markdown_core_mem *mem = capture->mem;
    size_t read;
    size_t write = 0;

    /* A noted span implies records: the reducer that noted it pushed its
     * endpoint records first. */
    if (capture->retraction_count) {
        capture_apply_retractions(capture);
    }
    if (capture->retractions) {
        mem->free(mem, capture->retractions);
        capture->retractions = NULL;
        capture->retraction_count = 0;
        capture->retraction_capacity = 0;
    }
    capture->records = NULL;
    if (!records) {
        capture->dirty = false;
        return NULL;
    }
    if (capture->dirty) {
        for (read = 0; read < records->count; read++) {
            if (records->records[read].flags & INLINE_CONCRETE_TOMBSTONE) {
                continue;
            }
            records->records[write++] = records->records[read];
        }
        records->count = write;
        capture->dirty = false;
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
    if (capture->retractions) {
        capture->mem->free(capture->mem, capture->retractions);
        capture->retractions = NULL;
        capture->retraction_count = 0;
        capture->retraction_capacity = 0;
    }
    capture->dirty = false;
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
