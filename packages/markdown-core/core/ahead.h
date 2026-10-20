#ifndef MARKDOWN_CORE_AHEAD_H
#define MARKDOWN_CORE_AHEAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A CONTIGUOUS ARRAY THAT MOVES AHEAD OF ITS GROWTH, so that no growth copies
 * it whole. Once the array is half full it takes storage of twice its
 * capacity, and as it grows on it copies its elements there from the front,
 * two for each place it gains, a slice at a time. When it is full it switches
 * to that storage, and only what was not copied moves then. Readers read the
 * array where it is, whole and contiguous, as any array's.
 *
 * Its owner keeps the array's ROOM, the count it can reach before it asks to
 * grow again, as an array's owner keeps its capacity: a push that passes the
 * room asks (markdown_core_ahead_extend), and every other push only writes.
 * The room is the capacity of an array of at most two slices; for a larger
 * one, half its capacity until it is half full, then the end of each slice.
 *
 * A copy holds while its element stays as it was. So an owner that writes an
 * element in place, or drops elements off the end and may push others where
 * they were, says so first (markdown_core_ahead_unsettle), and the copies from
 * that element on are made again. An array whose writes and truncations keep
 * to its last elements copies its settled front once, and the switch moves
 * the rest.
 *
 * The copies keep pace: from half full to full the array gains half its
 * capacity, so two copies for each place gained reach the whole capacity by
 * the switch. Over a parse each element is copied once at each capacity it
 * passes, as doubling copies it, and again after each write that reaches
 * it. An array of at most two slices moves whole when it is full, where the
 * allocator can extend it: the move copies no more than a slice of a larger
 * array does, and an array that stays small takes no storage ahead. */
typedef struct markdown_core_ahead {
    /* The places the array's storage has; the storage the array moves to, or
     * NULL, and the elements from the front that hold there as they hold in
     * the array. */
    size_t capacity;
    void *next;
    size_t moved;
} markdown_core_ahead;

/* The places an array gains between two copies. */
#define MARKDOWN_CORE_AHEAD_SLICE 64

/* The capacity an empty array first takes. */
#define MARKDOWN_CORE_AHEAD_FIRST 8

/* The room of an array with `capacity` places that holds `count` elements. */
static inline size_t markdown_core_ahead_room(size_t capacity, size_t count) {
    if (capacity <= 2 * MARKDOWN_CORE_AHEAD_SLICE) {
        return capacity;
    }
    if (count <= capacity / 2) {
        return capacity / 2;
    }
    const size_t end = count | (MARKDOWN_CORE_AHEAD_SLICE - 1);
    return end < capacity ? end : capacity;
}

/* WHERE AN ARRAY'S STORAGE COMES FROM: `take` gives storage for `capacity`
 * elements of `width` bytes, or NULL when none can be had, and `release` takes
 * back storage `take` gave, each with `pool`. An array whose source is
 * given as NULL takes its storage from the allocator (alloc.h). */
typedef struct markdown_core_ahead_source {
    void *(*take)(void *pool, size_t capacity, size_t width);
    void (*release)(void *pool, void *values);
    void *pool;
} markdown_core_ahead_source;

/* Gives back the storage the array was moving to, to `source`. */
static inline void markdown_core_ahead_free(markdown_core_ahead *ahead, const markdown_core_ahead_source *source) {
    if (ahead->next) {
        if (source) {
            source->release(source->pool, ahead->next);
        } else {
            markdown_core_free(ahead->next);
        }
    }
    const markdown_core_ahead empty = {0, NULL, 0};
    *ahead = empty;
}

/* markdown_core_ahead_extend past half full, short of the capacity: copies
 * ahead. */
void *markdown_core_ahead_copy(markdown_core_ahead *ahead, const markdown_core_ahead_source *source, void *values,
                               size_t *room, size_t held, size_t count, size_t width);
/* markdown_core_ahead_extend past the capacity, into the storage ahead, of
 * twice the capacity: switches. */
void *markdown_core_ahead_switch(markdown_core_ahead *ahead, const markdown_core_ahead_source *source, void *values,
                                 size_t *room, size_t held, size_t count, size_t width);

/* Room for `count` elements of `width` bytes, more than `*room`, in the array
 * `values` holds `held` of, moving by `ahead`, its storage from `source`.
 * The array's storage, which moved when the array was full, with `*room` its
 * room for `count`; NULL, with the array where it was, when storage could
 * not be had. An array with no storage ahead of twice its capacity moves
 * whole: from the allocator, which extends it in place where it can. */
static inline void *markdown_core_ahead_extend(markdown_core_ahead *ahead, const markdown_core_ahead_source *source,
                                               void *values, size_t *room, size_t held, size_t count, size_t width) {
    const size_t capacity = ahead->capacity;
    if (count <= capacity) {
        return markdown_core_ahead_copy(ahead, source, values, room, held, count, width);
    }
    if (count > SIZE_MAX / 2 / width) {
        return NULL;
    }
    size_t larger = capacity ? capacity : MARKDOWN_CORE_AHEAD_FIRST;
    while (larger < count) {
        larger *= 2;
    }
    if (ahead->next && larger == 2 * capacity) {
        return markdown_core_ahead_switch(ahead, source, values, room, held, count, width);
    }
    void *moved;
    if (source) {
        if (!(moved = source->take(source->pool, larger, width))) {
            return NULL;
        }
        if (held) {
            memcpy(moved, values, held * width);
        }
        if (values) {
            source->release(source->pool, values);
        }
    } else if (!(moved = markdown_core_realloc(values, larger * width))) {
        return NULL;
    }
    markdown_core_ahead_free(ahead, source);
    ahead->capacity = larger;
    *room = markdown_core_ahead_room(larger, count);
    return moved;
}

/* The elements from `first` on change: the copies of them are void. */
static inline void markdown_core_ahead_unsettle(markdown_core_ahead *ahead, size_t first) {
    if (ahead->moved > first) {
        ahead->moved = first;
    }
}

#ifdef __cplusplus
}
#endif

#endif
