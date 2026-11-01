#include <assert.h>
#include "ahead.h"
#include "array.h"

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
    assert(capacity > 2 * markdown_core_ahead_slice(width) && count > capacity / 2);
    const size_t pace = 2 * (count - capacity / 2);
    const size_t end = pace < held ? pace : held;
    /* The storage ahead is taken once there is more than a slice to copy:
     * the switch moves what is less, as the move of a small array does. */
    if (end > ahead->moved && held > markdown_core_ahead_slice(width)) {
        if (!ahead->next && (capacity > SIZE_MAX / 2 / width || !(ahead->next = S_take(source, 2 * capacity, width)))) {
            return NULL;
        }
        memcpy((unsigned char *)ahead->next + ahead->moved * width, (unsigned char *)values + ahead->moved * width,
               (end - ahead->moved) * width);
        ahead->moved = end;
    }
    *room = markdown_core_ahead_room(capacity, count, width);
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
    *room = markdown_core_ahead_room(larger, count, width);
    return moved;
}

void *markdown_core_array_extend(markdown_core_ahead *ahead, void *values, size_t *room, size_t held, size_t count,
                                 size_t width) {
    void *moved = markdown_core_ahead_extend(ahead, NULL, values, room, held, count, width);
    return moved ? moved : values;
}

void *markdown_core_array_fit(markdown_core_ahead *ahead, void *values, size_t *room, size_t count, size_t width) {
    size_t capacity = MARKDOWN_CORE_AHEAD_FIRST;
    while (capacity < count) {
        capacity *= 2;
    }
    if (!values || capacity >= ahead->capacity) {
        return values;
    }
    void *moved = markdown_core_realloc(values, capacity * width);
    if (!moved) {
        return values;
    }
    markdown_core_ahead_free(ahead, NULL);
    ahead->capacity = capacity;
    *room = markdown_core_ahead_room(capacity, count, width);
    return moved;
}
