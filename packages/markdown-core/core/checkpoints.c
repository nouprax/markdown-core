#include <string.h>

#include "alloc.h"
#include "checkpoints.h"
#include "node.h"

/* A chunk of storage: the entries, frames and checkpoints of one session are
 * carved from it in turn and never move. The first chunk holds about what a
 * few kilobytes of document take, and each one after it twice the one
 * before, up to CHUNK_UNITS. */
#define CHUNK_FIRST ((size_t)256)
#define CHUNK_UNITS ((size_t)4096)

/* The unit storage is carved in, aligned for any of them. */
typedef union {
    void *pointer;
    uint64_t word;
    long double number;
} chunk_unit;

typedef struct markdown_core_checkpoint_chunk {
    struct markdown_core_checkpoint_chunk *next;
    size_t used, capacity;
    chunk_unit units[];
} markdown_core_checkpoint_chunk;

static void *carve(markdown_core_checkpoints *store, size_t size) {
    size = (size + sizeof(chunk_unit) - 1) / sizeof(chunk_unit);
    markdown_core_checkpoint_chunk *chunk = store->chunks;
    if (!chunk || chunk->used + size > chunk->capacity) {
        size_t capacity = chunk ? 2 * chunk->capacity : CHUNK_FIRST;
        capacity = capacity < CHUNK_UNITS ? capacity : CHUNK_UNITS;
        chunk = markdown_core_realloc(NULL, sizeof(*chunk) + capacity * sizeof(chunk_unit));
        if (!chunk) {
            return NULL;
        }
        chunk->next = store->chunks;
        chunk->used = 0;
        chunk->capacity = capacity;
        store->chunks = chunk;
    }
    void *made = chunk->units + chunk->used;
    chunk->used += size;
    return made;
}

markdown_core_entry *markdown_core_entry_new(markdown_core_checkpoints *store, markdown_core_node *node) {
    markdown_core_entry *entry = store->free_entries;
    if (entry) {
        store->free_entries = (markdown_core_entry *)(void *)entry->link.up;
    } else if (!(entry = carve(store, sizeof(*entry)))) {
        return NULL;
    }
    memset(entry, 0, sizeof(*entry));
    entry->node = node;
    node->entry = entry;
    return entry;
}

void markdown_core_entry_drop(markdown_core_checkpoints *store, markdown_core_entry *entry) {
    if (!entry || --entry->holds) {
        return;
    }
    if (entry->placed) {
        markdown_core_summed_take(&store->entries, &entry->link);
    }
    if (entry->node) {
        entry->node->entry = NULL;
    }
    entry->link.up = (markdown_core_summed_node *)(void *)store->free_entries;
    store->free_entries = entry;
}

markdown_core_frame *markdown_core_frame_new(markdown_core_checkpoints *store, markdown_core_frame *parent,
                                             markdown_core_entry *entry, uint16_t bits, uint64_t carry) {
    markdown_core_frame *frame = store->free_frames;
    if (frame) {
        store->free_frames = frame->parent;
    } else if (!(frame = carve(store, sizeof(*frame)))) {
        return NULL;
    }
    *frame = (markdown_core_frame){parent, entry, 0, bits, carry};
    markdown_core_frame_hold(parent);
    markdown_core_entry_hold(entry);
    return frame;
}

void markdown_core_frame_drop(markdown_core_checkpoints *store, markdown_core_frame *frame) {
    while (frame && !--frame->holds) {
        markdown_core_frame *parent = frame->parent;
        markdown_core_entry_drop(store, frame->entry);
        frame->parent = store->free_frames;
        store->free_frames = frame;
        frame = parent;
    }
}

markdown_core_checkpoint *markdown_core_checkpoint_new(markdown_core_checkpoints *store) {
    markdown_core_checkpoint *checkpoint = store->free_checkpoints;
    if (checkpoint) {
        store->free_checkpoints = (markdown_core_checkpoint *)(void *)checkpoint->link.up;
    } else if (!(checkpoint = carve(store, sizeof(*checkpoint)))) {
        return NULL;
    }
    memset(checkpoint, 0, sizeof(*checkpoint));
    return checkpoint;
}

void markdown_core_checkpoint_free(markdown_core_checkpoints *store, markdown_core_checkpoint *checkpoint) {
    markdown_core_frame_drop(store, checkpoint->frame);
    markdown_core_entry_drop(store, checkpoint->below);
    checkpoint->link.up = (markdown_core_summed_node *)(void *)store->free_checkpoints;
    store->free_checkpoints = checkpoint;
}

/* Visits every element of a tree in order, each after the walk has left it,
 * so `visit` may free it; a balanced tree is under 64 levels high. */
static void visit_all(markdown_core_checkpoints *store, markdown_core_summed_node *link,
                      void (*visit)(markdown_core_checkpoints *, markdown_core_summed_node *)) {
    markdown_core_summed_node *stack[64];
    size_t depth = 0;
    while (link || depth) {
        while (link) {
            stack[depth++] = link;
            link = link->before;
        }
        link = stack[--depth];
        markdown_core_summed_node *after = link->after;
        visit(store, link);
        link = after;
    }
}

static void unplace(markdown_core_checkpoints *store, markdown_core_summed_node *link) {
    (void)store;
    markdown_core_entry_of(link)->placed = false;
}

static void release(markdown_core_checkpoints *store, markdown_core_summed_node *link) {
    markdown_core_checkpoint_free(store, markdown_core_checkpoint_of(link));
}

void markdown_core_checkpoints_clear(markdown_core_checkpoints *store) {
    /* The entries leave their tree at once, and then every one goes as the
     * last checkpoint that names it does. */
    visit_all(store, store->entries.root, unplace);
    store->entries.root = NULL;
    visit_all(store, store->lines.root, release);
    store->lines.root = NULL;
    store->touched_start = store->touched_end = 0;
}

void markdown_core_checkpoints_dispose(markdown_core_checkpoints *store) {
    markdown_core_checkpoints_clear(store);
    while (store->chunks) {
        markdown_core_checkpoint_chunk *chunk = store->chunks;
        store->chunks = chunk->next;
        markdown_core_free(chunk);
    }
    memset(store, 0, sizeof(*store));
}
