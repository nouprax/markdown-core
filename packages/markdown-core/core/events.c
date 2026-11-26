#include <string.h>

#include "alloc.h"
#include "events.h"

void markdown_core_events_init(markdown_core_events *events) { *events = (markdown_core_events){0}; }

void markdown_core_events_free(markdown_core_events *events) {
    markdown_core_free(events->tokens);
    markdown_core_free(events->items);
    markdown_core_free(events->kept);
    *events = (markdown_core_events){0};
}

void markdown_core_events_clear(markdown_core_events *events) {
    events->token_count = 0;
    events->count = 0;
    events->kept_size = 0;
    events->failed = false;
}

uint32_t markdown_core_events_keep(markdown_core_events *events, const void *record, size_t size) {
    /* Records start where any type may. */
    const size_t at = (events->kept_size + 7u) & ~(size_t)7u;
    unsigned char *kept = events->failed ? NULL : markdown_core_reserve(events->kept, &events->kept_room, at + size, 1);
    if (!kept || at + size > UINT32_MAX) {
        events->failed = true;
        return UINT32_MAX;
    }
    events->kept = kept;
    memcpy(kept + at, record, size);
    events->kept_size = at + size;
    return (uint32_t)at;
}

uint32_t markdown_core_events_token(markdown_core_events *events, uint16_t kind, uint32_t start) {
    if (events->failed) {
        return UINT32_MAX;
    }
    markdown_core_token *tokens =
        markdown_core_reserve(events->tokens, &events->token_room, events->token_count + 1, sizeof(*tokens));
    if (tokens) {
        events->tokens = tokens;
    }
    if (!tokens || events->token_count >= (size_t)INT32_MAX) {
        events->failed = true;
        return UINT32_MAX;
    }
    tokens[events->token_count] = (markdown_core_token){.kind = kind, .start = start, .end = start};
    return (uint32_t)events->token_count++;
}

void markdown_core_events_push(markdown_core_events *events, markdown_core_phase phase, uint32_t token) {
    if (events->failed || token == UINT32_MAX) {
        events->failed = true;
        return;
    }
    markdown_core_event *items = markdown_core_reserve(events->items, &events->room, events->count + 1, sizeof(*items));
    if (!items) {
        events->failed = true;
        return;
    }
    events->items = items;
    items[events->count++] = (markdown_core_event){.phase = phase, .token = token};
}

void markdown_core_events_splice(markdown_core_events *events, size_t at, size_t removed,
                                 const markdown_core_event *items, size_t added) {
    if (events->failed) {
        return;
    }
    if (added > removed) {
        markdown_core_event *wider =
            markdown_core_reserve(events->items, &events->room, events->count + (added - removed), sizeof(*wider));
        if (!wider) {
            events->failed = true;
            return;
        }
        events->items = wider;
    }
    const size_t tail = events->count - at - removed;
    if (tail && added != removed) {
        memmove(events->items + at + added, events->items + at + removed, tail * sizeof(*events->items));
    }
    if (added) {
        memcpy(events->items + at, items, added * sizeof(*items));
    }
    events->count = events->count - removed + added;
}

void markdown_core_events_cut(markdown_core_events *events, size_t count, size_t tokens) {
    if (count < events->count) {
        events->count = count;
    }
    if (tokens < events->token_count) {
        events->token_count = tokens;
    }
}
