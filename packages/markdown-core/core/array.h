#ifndef MARKDOWN_CORE_ARRAY_H
#define MARKDOWN_CORE_ARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include "ahead.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A GROWING ARRAY: the one type of the buffers a stream grows at their end,
 * an array of `type` that holds its storage, the `count` elements it holds,
 * its room and the storage it moves to (ahead.h). Each of the session's
 * text, the line index, the content marks and the settled nodes is an array
 * of it, and a block's content buffer grows by the same operations
 * (buffer.h, markdown_core_strbuf_put_ahead).
 *
 * Readers read `values[0 .. count)` in place. Writers go through the
 * operations below, which keep the copies ahead in step with the elements:
 * a push or an append writes past the end, a write gives an element to
 * change in place, and a truncation drops elements off the end. An element
 * written any other way leaves a copy that no longer holds, which the switch
 * asserts (ahead.c). The array is zeroed when it is empty, and each
 * operation evaluates its array more than once. */
#define MARKDOWN_CORE_ARRAY(type)                                                                                      \
    struct {                                                                                                           \
        type *values;                                                                                                  \
        size_t count, room;                                                                                            \
        markdown_core_ahead ahead;                                                                                     \
    }

/* markdown_core_ahead_extend for an array whose storage comes from the
 * allocator: the array's storage, `values` itself with `*room` as it was
 * when storage could not be had. Out of line: it runs once in a slice. */
void *markdown_core_array_extend(markdown_core_ahead *ahead, void *values, size_t *room, size_t held, size_t count,
                                 size_t width);

/* Whether the array has room for `total` elements, which it makes when it
 * has less: false, with the array as it was, when storage could not be
 * had. */
#define markdown_core_array_reserve(array, total)                                                                      \
    ((total) <= (array)->room ||                                                                                       \
     ((array)->values = markdown_core_array_extend(&(array)->ahead, (array)->values, &(array)->room, (array)->count,   \
                                                   (total), sizeof(*(array)->values)),                                 \
      (total) <= (array)->room))

/* Pushes `value`: whether it had room for it. */
#define markdown_core_array_push(array, value)                                                                         \
    (markdown_core_array_reserve((array), (array)->count + 1) && ((array)->values[(array)->count++] = (value), true))

/* Appends the `size` elements at `source`, which lies outside the array's
 * storage unless the array has room for them already: whether it had room
 * for them. */
#define markdown_core_array_append(array, source, size)                                                                \
    (markdown_core_array_reserve((array), (array)->count + (size)) &&                                                  \
     (memcpy((array)->values + (array)->count, (source), (size) * sizeof(*(array)->values)), (array)->count += (size), \
      true))

/* Room for `total` elements in an array whose storage also holds elements
 * past its count, up to `used`, which its user writes in place: they move
 * with the array, but no copy of them holds once it is made. */
#define markdown_core_array_reserve_past(array, used, total)                                                           \
    (markdown_core_ahead_unsettle(&(array)->ahead, (array)->count),                                                    \
     (total) <= (array)->room ||                                                                                       \
         ((array)->values = markdown_core_array_extend(&(array)->ahead, (array)->values, &(array)->room, (used),       \
                                                       (total), sizeof(*(array)->values)),                             \
          (total) <= (array)->room))

/* Counts the elements written past the count, up to `size`, as the array's
 * own: they stay as they are from now on. */
#define markdown_core_array_take(array, size)                                                                          \
    (markdown_core_ahead_unsettle(&(array)->ahead, (array)->count), (void)((array)->count = (size)))

/* The element at `index`, to change in place. */
#define markdown_core_array_write(array, index)                                                                        \
    (markdown_core_ahead_unsettle(&(array)->ahead, (index)), &(array)->values[(index)])

/* Drops the elements from `size` on. */
#define markdown_core_array_truncate(array, size)                                                                      \
    (markdown_core_ahead_unsettle(&(array)->ahead, (size)), (void)((array)->count = (size)))

/* markdown_core_array_shrink's storage: the array's, moved to the least
 * capacity that holds `count`, or `values` itself as it was when that is
 * no less or storage could not be had. Out of line: a session that lets
 * most of its text go runs it once. */
void *markdown_core_array_fit(markdown_core_ahead *ahead, void *values, size_t *room, size_t count, size_t width);

/* Gives back the storage the array holds past what its elements need, with
 * the storage ahead: when it cannot, it stays as it was. */
#define markdown_core_array_shrink(array)                                                                              \
    ((array)->values = markdown_core_array_fit(&(array)->ahead, (array)->values, &(array)->room, (array)->count,       \
                                               sizeof(*(array)->values)))

/* Gives back the array's storage, which leaves it empty. */
#define markdown_core_array_free(array)                                                                                \
    (markdown_core_free((array)->values), markdown_core_ahead_free(&(array)->ahead, NULL), (array)->values = NULL,     \
     (void)((array)->count = (array)->room = 0))

#ifdef __cplusplus
}
#endif

#endif
