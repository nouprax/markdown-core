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
/* A value holding one class, `bytes`, and nothing else: the value an element
 * makes when its syntax names a class without an attribute container. The
 * caller supplies an empty value. Returns 0 on allocation failure, leaving it
 * empty. */
int markdown_core_attributes_single_class(markdown_core_attributes *value, const unsigned char *bytes,
                                          bufsize_t length);

#endif
