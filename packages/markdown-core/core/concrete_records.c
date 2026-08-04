#include "concrete_records.h"

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
