#include <assert.h>
#include "ahead.h"

static void *S_take(const markdown_core_ahead_source *source, size_t capacity, size_t width) {
    return source ? source->take(source->pool, capacity, width) : markdown_core_realloc(NULL, capacity * width);
}

static void S_release(const markdown_core_ahead_source *source, void *values) {
    if (source) {
        source->release(source->pool, values);
    } else {
        markdown_core_free(values);
    }
}

void *markdown_core_ahead_copy(markdown_core_ahead *ahead, const markdown_core_ahead_source *source, void *values,
                               size_t *room, size_t held, size_t count, size_t width) {
    const size_t capacity = ahead->capacity;
    assert(ahead->moved <= held && held <= count && count > *room && count <= capacity);
    assert(capacity > 2 * MARKDOWN_CORE_AHEAD_SLICE && count > capacity / 2);
    if (!ahead->next) {
        if (capacity > SIZE_MAX / 2 / width || !(ahead->next = S_take(source, 2 * capacity, width))) {
            return NULL;
        }
    }
    const size_t pace = 2 * (count - capacity / 2);
    const size_t end = pace < held ? pace : held;
    if (end > ahead->moved) {
        memcpy((unsigned char *)ahead->next + ahead->moved * width, (unsigned char *)values + ahead->moved * width,
               (end - ahead->moved) * width);
        ahead->moved = end;
    }
    *room = markdown_core_ahead_room(capacity, count);
    return values;
}

void *markdown_core_ahead_switch(markdown_core_ahead *ahead, const markdown_core_ahead_source *source, void *values,
                                 size_t *room, size_t held, size_t count, size_t width) {
    const size_t larger = 2 * ahead->capacity;
    unsigned char *const moved = ahead->next;
    assert(moved && ahead->moved <= held && held <= ahead->capacity && ahead->capacity < count && count <= larger);
    /* Every copy the owner did not void holds. */
    assert(!memcmp(moved, values, ahead->moved * width));
    memcpy(moved + ahead->moved * width, (unsigned char *)values + ahead->moved * width, (held - ahead->moved) * width);
    S_release(source, values);
    *ahead = (markdown_core_ahead){.capacity = larger};
    *room = markdown_core_ahead_room(larger, count);
    return moved;
}
