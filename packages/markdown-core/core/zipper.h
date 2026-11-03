#ifndef MARKDOWN_CORE_ZIPPER_H
#define MARKDOWN_CORE_ZIPPER_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "alloc.h"
#include "array.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A ZIPPER (docs/plans/edit-linear-costs.md): entries in order of where
 * they start in a text, held in two parts about a focus, the place an edit
 * reads from. The FRONT holds the entries before the focus, each place in it
 * where it is in the text, in an array a parse reads and grows in place
 * (array.h), so that a reading of it costs what a reading of the array does.
 * The BACK holds the rest, each place in it measured back from the text's
 * end, `end`, in a stack whose top is the entry nearest the focus: an edit
 * before them moves none of them, only the end. Moving the focus moves only
 * the entries between where it was and where it goes, and a splice drops the
 * back's entries before a place.
 *
 * A zipper of entries of `type` is MARKDOWN_CORE_ZIPPER(type), and
 * MARKDOWN_CORE_ZIPPER_OPS makes its operations, each a function of its own
 * type, from what the type gives (the interface):
 *
 *   flip(entry, end)  the entry with each place p it holds made end - p,
 *                     which turns a front entry into a back one and a back
 *                     one into a front one; what only a front entry holds
 *                     it drops;
 *   start(entry)      the place it is in order of, in its part's terms;
 *   leave(owner, i)   called before the front's entries from `i` on leave
 *                     it, to the back or for good, or move in it: what the
 *                     instance keeps about them, an index over the front,
 *                     lets them go;
 *   turn(owner, z, e) called for each entry `e` a move of the focus took to
 *                     the other part, there, with the zipper `z` as it then
 *                     is: what the instance keeps of where its entries are,
 *                     an index by another key, follows it.
 *
 * The back is the zipper's own; a parse that keeps no back -- one that no
 * edit put back -- never touches it. */
#define MARKDOWN_CORE_ZIPPER(type)                                                                                     \
    struct {                                                                                                           \
        MARKDOWN_CORE_ARRAY(type) front;                                                                               \
        type *back;                                                                                                    \
        size_t back_count, back_room, end;                                                                             \
    }

/* The `turn` of an instance that keeps nothing of where its entries are. */
#define MARKDOWN_CORE_ZIPPER_NO_TURN(self, z, entry) ((void)(self), (void)(z), (void)(entry))

/* The operations of the zipper type `zipper` of entries of `type`, named
 * `name_...`, `flip`, `start`, `leave` and `turn` as above, `owner` the type
 * of what `leave` and `turn` are called with. */
#define MARKDOWN_CORE_ZIPPER_OPS(name, zipper, type, flip, start, leave, turn, owner)                                  \
    /* How many entries the zipper holds. */                                                                           \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_count(const zipper *z) {                             \
        return z->front.count + z->back_count;                                                                         \
    }                                                                                                                  \
                                                                                                                       \
    /* The back's entry `k` from the focus, in the text's terms. */                                                    \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) type name##_back_at(const zipper *z, size_t k) {                   \
        assert(k < z->back_count);                                                                                     \
        return flip(z->back[z->back_count - 1 - k], z->end);                                                           \
    }                                                                                                                  \
                                                                                                                       \
    /* Where the back's entry `k` from the focus starts in the text. */                                                \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_back_start(const zipper *z, size_t k) {              \
        assert(k < z->back_count);                                                                                     \
        return z->end - start(&z->back[z->back_count - 1 - k]);                                                        \
    }                                                                                                                  \
                                                                                                                       \
    /* Entry `index`, below the count, in the text's terms. */                                                         \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) type name##_at(const zipper *z, size_t index) {                    \
        return index < z->front.count ? z->front.values[index] : name##_back_at(z, index - z->front.count);            \
    }                                                                                                                  \
                                                                                                                       \
    /* Where entry `index` starts in the text. */                                                                      \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_start(const zipper *z, size_t index) {               \
        return index < z->front.count ? start(&z->front.values[index]) : name##_back_start(z, index - z->front.count); \
    }                                                                                                                  \
                                                                                                                       \
    /* The first of the back's entries, from the focus, that starts at or                                              \
     * past `place` of the text, or the back's count. */                                                               \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_back_from(const zipper *z, size_t place) {           \
        size_t low = 0, high = z->back_count;                                                                          \
        while (low < high) {                                                                                           \
            const size_t middle = low + (high - low) / 2;                                                              \
            if (name##_back_start(z, middle) < place) {                                                                \
                low = middle + 1;                                                                                      \
            } else {                                                                                                   \
                high = middle;                                                                                         \
            }                                                                                                          \
        }                                                                                                              \
        return low;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* The first of the front's entries that starts at or past `place`, or                                             \
     * the front's count. */                                                                                           \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_front_from(const zipper *z, size_t place) {          \
        size_t low = 0, high = z->front.count;                                                                         \
        while (low < high) {                                                                                           \
            const size_t middle = low + (high - low) / 2;                                                              \
            if (start(&z->front.values[middle]) < place) {                                                             \
                low = middle + 1;                                                                                      \
            } else {                                                                                                   \
                high = middle;                                                                                         \
            }                                                                                                          \
        }                                                                                                              \
        return low;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* The first entry that starts at or past `place`, or the count: every                                             \
     * entry of the front starts before every entry of the back. */                                                    \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_from(const zipper *z, size_t place) {                \
        if (z->back_count && name##_back_start(z, 0) < place) {                                                        \
            return z->front.count + name##_back_from(z, place);                                                        \
        }                                                                                                              \
        return name##_front_from(z, place);                                                                            \
    }                                                                                                                  \
                                                                                                                       \
    /* Room for `count` entries in the back. False, with the zipper as it                                              \
     * was, when storage could not be had. */                                                                          \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) bool name##_reserve_back(zipper *z, size_t count) {                \
        if (count <= z->back_room) {                                                                                   \
            return true;                                                                                               \
        }                                                                                                              \
        type *back = markdown_core_reserve(z->back, &z->back_room, count, sizeof(type));                               \
        if (!back) {                                                                                                   \
            return false;                                                                                              \
        }                                                                                                              \
        z->back = back;                                                                                                \
        return true;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* The front's entries from `first` on go, `leave` told first. */                                                  \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) void name##_truncate(zipper *z, size_t first, owner *self) {       \
        if (first < z->front.count) {                                                                                  \
            leave(self, first);                                                                                        \
            markdown_core_array_truncate(&z->front, first);                                                            \
        }                                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    /* Room for the move to `place` (name##_move), which then cannot fail.                                             \
     * False, with the zipper as it was, when storage could not be had. */                                             \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) bool name##_ready(zipper *z, size_t place, size_t size) {          \
        assert(!z->back_count || z->end == size);                                                                      \
        (void)size;                                                                                                    \
        const size_t first = name##_front_from(z, place);                                                              \
        const size_t taken = z->back_count ? name##_back_from(z, place) : 0;                                           \
        return name##_reserve_back(z, z->back_count + z->front.count - first) &&                                       \
               markdown_core_array_reserve(&z->front, z->front.count + taken);                                         \
    }                                                                                                                  \
                                                                                                                       \
    /* Moves the focus to `place` of a text of `size` bytes, which ends where                                          \
     * the back's text ends: the front's entries that start there or past it                                           \
     * go to the back, or the back's that start before it come to the front,                                           \
     * as many as move. False, with the zipper as it was, when storage could                                           \
     * not be had. */                                                                                                  \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) bool name##_move(zipper *z, size_t place, size_t size,             \
                                                                     owner *self) {                                    \
        if (!name##_ready(z, place, size)) {                                                                           \
            return false;                                                                                              \
        }                                                                                                              \
        z->end = size;                                                                                                 \
        const size_t first = name##_front_from(z, place);                                                              \
        const size_t taken = z->back_count ? name##_back_from(z, place) : 0;                                           \
        if (first < z->front.count) {                                                                                  \
            leave(self, first);                                                                                        \
            const size_t count = z->front.count;                                                                       \
            markdown_core_array_truncate(&z->front, first);                                                            \
            for (size_t i = count; i-- > first;) {                                                                     \
                z->back[z->back_count] = flip(z->front.values[i], z->end);                                             \
                turn(self, z, &z->back[z->back_count++]);                                                              \
            }                                                                                                          \
        }                                                                                                              \
        for (size_t i = 0; i < taken; i++) {                                                                           \
            z->front.values[z->front.count] = flip(z->back[--z->back_count], z->end);                                  \
            turn(self, z, &z->front.values[z->front.count++]);                                                         \
        }                                                                                                              \
        return true;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* The back's entries that start before `place` of the text go. */                                                 \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) size_t name##_cut(zipper *z, size_t place) {                       \
        const size_t gone = name##_back_from(z, place);                                                                \
        z->back_count -= gone;                                                                                         \
        return gone;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* Its front's entry `index` goes, the others staying in order: those                                              \
     * after it move, so `leave` hears of all of them. */                                                              \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) void name##_front_remove(zipper *z, size_t index, owner *self) {   \
        leave(self, index);                                                                                            \
        markdown_core_ahead_unsettle(&z->front.ahead, index);                                                          \
        memmove(z->front.values + index, z->front.values + index + 1, (z->front.count - index - 1) * sizeof(type));    \
        markdown_core_array_truncate(&z->front, z->front.count - 1);                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* `entry` put in the front at `index`, the entries from there on after                                            \
     * it, `leave` told first. False, with the zipper as it was, when storage                                          \
     * could not be had. */                                                                                            \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) bool name##_front_insert(zipper *z, size_t index, type entry,      \
                                                                             owner *self) {                            \
        if (!markdown_core_array_reserve(&z->front, z->front.count + 1)) {                                             \
            return false;                                                                                              \
        }                                                                                                              \
        if (index < z->front.count) {                                                                                  \
            leave(self, index);                                                                                        \
            markdown_core_ahead_unsettle(&z->front.ahead, index);                                                      \
            memmove(z->front.values + index + 1, z->front.values + index, (z->front.count - index) * sizeof(type));    \
        }                                                                                                              \
        z->front.values[index] = entry;                                                                                \
        z->front.count++;                                                                                              \
        return true;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* Its back's entry `k` from the focus goes, the others staying in order. */                                       \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) void name##_back_remove(zipper *z, size_t k) {                     \
        const size_t at = z->back_count - 1 - k;                                                                       \
        memmove(z->back + at, z->back + at + 1, k * sizeof(type));                                                     \
        z->back_count--;                                                                                               \
    }                                                                                                                  \
                                                                                                                       \
    /* Its back's storage let go, which leaves the back empty. */                                                      \
    static inline MARKDOWN_CORE_ATTRIBUTE((unused)) void name##_release_back(zipper *z) {                              \
        markdown_core_free(z->back);                                                                                   \
        z->back = NULL;                                                                                                \
        z->back_count = z->back_room = 0;                                                                              \
    }

#ifdef __cplusplus
}
#endif

#endif
