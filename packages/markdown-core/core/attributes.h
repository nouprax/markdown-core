#ifndef MARKDOWN_CORE_ATTRIBUTES_H
#define MARKDOWN_CORE_ATTRIBUTES_H

#include <stdint.h>

#include "buffer.h"
#include "chunk.h"

typedef struct {
    markdown_core_chunk name;
    markdown_core_chunk value;
} markdown_core_record;

/* One normalized value. Empty anchor bytes mean no anchor. The lists retain
 * every occurrence.
 *
 * A VALUE IS ONE ALLOCATION. Its records, its classes and every string they
 * and the anchor name live in `storage`, laid out once the whole container
 * has been read, and they begin and end with the value: no chunk in it owns
 * anything of its own. The anchor is the one member a consumer may replace
 * with a string it made (a chunk with `alloc` set), and the release frees
 * such an anchor beside `storage`. Every string in `storage` is
 * NUL-terminated. */
typedef struct markdown_core_attribute_value {
    markdown_core_chunk anchor;
    markdown_core_chunk *classes;
    markdown_core_record *records;
    /* Each class and record is at least one byte of its container, so a
     * count is bounded like any length of the input. */
    uint32_t class_count;
    uint32_t record_count;
    void *storage;
} markdown_core_attributes;

/* Whether a value owns anything a release must free: its block, or an anchor
 * string of its own, so a value with neither owns nothing.
 * Almost every value is empty -- every node carries one and no Text has
 * attributes -- so this is the first test a release makes, shared with the
 * node release that makes it in place before calling. */
static MARKDOWN_CORE_INLINE bool markdown_core_attributes_owns(const markdown_core_attributes *value) {
    return value->storage || value->anchor.data;
}
/* Releases what the value owns, leaving it empty. */
void markdown_core_attributes_free(markdown_core_attributes *value);
/* A VIEW OF A VALUE holds the value's members and owns none of them: it
 * reads the value's block and anchor for as long as the value keeps them, and
 * releasing it frees nothing. `value`, a copy of another value's members,
 * becomes a view of that value. */
static MARKDOWN_CORE_INLINE void markdown_core_attributes_view(markdown_core_attributes *value) {
    value->storage = NULL;
    value->anchor.alloc = 0;
}
/* What `view` still reads of `value`, of which it is a view, copied: the
 * members it kept in `value`'s block, into a block of its own, and `value`'s
 * anchor string where it kept it. `view` then owns everything it holds.
 * False when storage could not be had, with what was not copied still read
 * from `value`. */
bool markdown_core_attributes_own(markdown_core_attributes *view, const markdown_core_attributes *value);
/* The same without a copy: `view` takes `value`'s block when it still reads
 * a member there, and `value`'s anchor string when it reads that, and owns
 * everything it holds; `value` then holds them without owning them. */
void markdown_core_attributes_adopt(markdown_core_attributes *view, markdown_core_attributes *value);

#endif
