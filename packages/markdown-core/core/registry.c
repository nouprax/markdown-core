#include <assert.h>
#include <string.h>
#include "alloc.h"
#include "registry.h"
#include "parser.h"
#include "utf8.h"

/* THE LOG'S RECORD STORAGE. A record lives until the registry is freed or a
 * rollback pops it, and pops come newest first, so records are carved one
 * after another from blocks the registry owns: a rollback releases the blocks
 * opened after its mark and resumes the mark's block where the mark left it.
 * A block starts small, so a parse that declares a few labels costs one small
 * allocation, and doubles up to a cap, so a document with many definitions
 * pays for its records in a few allocations rather than one each. A record
 * larger than the next block gets a block of its own size. */
struct markdown_core_registry_block {
    union {
        markdown_core_registry_block *next;
        long double alignment;
        int64_t integer_alignment;
    } head;
};

#define REGISTRY_BLOCK_MIN_BYTES ((size_t)1024)
#define REGISTRY_BLOCK_MAX_BYTES ((size_t)16 * 1024)

static void *S_carve(markdown_core_registry *registry, size_t size) {
    const size_t align = sizeof(markdown_core_registry_block);
    if (size > SIZE_MAX - align) {
        return NULL;
    }
    size = (size + align - 1) / align * align;
    if (!registry->blocks || registry->block_size - registry->block_used < size) {
        size_t capacity = registry->block_size ? registry->block_size * 2 : REGISTRY_BLOCK_MIN_BYTES;
        if (capacity > REGISTRY_BLOCK_MAX_BYTES) {
            capacity = REGISTRY_BLOCK_MAX_BYTES;
        }
        if (capacity < size) {
            capacity = size;
        }
        if (capacity > SIZE_MAX - sizeof(markdown_core_registry_block)) {
            return NULL;
        }
        markdown_core_registry_block *block = (markdown_core_registry_block *)markdown_core_realloc(
            NULL, sizeof(markdown_core_registry_block) + capacity);
        if (!block) {
            return NULL;
        }
        block->head.next = registry->blocks;
        registry->blocks = block;
        registry->block_size = capacity;
        registry->block_used = 0;
    }
    void *storage = (unsigned char *)(registry->blocks + 1) + registry->block_used;
    registry->block_used += size;
    return storage;
}

void markdown_core_registry_init(markdown_core_registry *registry) {
    *registry = (markdown_core_registry){0};
    markdown_core_strbuf_init(&registry->key, 0);
}

void markdown_core_registry_free(markdown_core_registry *registry) {
    while (registry->blocks) {
        markdown_core_registry_block *next = registry->blocks->head.next;
        markdown_core_free(registry->blocks);
        registry->blocks = next;
    }
    markdown_core_free(registry->records);
    markdown_core_key_index_free(&registry->index);
    markdown_core_strbuf_free(&registry->key);
    *registry = (markdown_core_registry){0};
    markdown_core_strbuf_init(&registry->key, 0);
}

bool markdown_core_registry_declare(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                    const unsigned char *label, bufsize_t length, uint32_t start) {
    if (length <= 0) {
        return true;
    }
    void *records =
        markdown_core_reserve(registry->records, &registry->capacity, registry->count + 1, sizeof(*registry->records));
    if (!records) {
        return false;
    }
    registry->records = records;
    markdown_core_registry_record *record = S_carve(registry, sizeof(*record) + (size_t)length + 2);
    if (!record) {
        return false;
    }
    record->key[0] = (unsigned char)kind;
    memcpy(record->key + 1, label, (size_t)length);
    record->key[length + 1] = '\0';
    record->key_len = length + 1;
    record->start = record->earliest = start;
    record->previous = 0;
    registry->records[registry->count++] = record;
    registry->spaces[kind].records++;
    return true;
}

/* The index takes the kind's records the log holds beyond its cursor, in log
 * order, with room made for all of them first. */
static bool S_index_space(markdown_core_registry *registry, markdown_core_registry_kind kind) {
    markdown_core_registry_space *space = &registry->spaces[kind];
    if (space->indexed < space->records &&
        !markdown_core_key_index_reserve(&registry->index, registry->index.size + space->records - space->indexed)) {
        return false;
    }
    for (; space->indexed < space->records; space->cursor++) {
        markdown_core_registry_record *record = registry->records[space->cursor];
        if (record->key[0] != (unsigned char)kind) {
            continue;
        }
        markdown_core_key_index_slot *slot =
            markdown_core_key_index_entry(&registry->index, record->key, record->key_len);
        if (!slot) {
            return false;
        }
        if (slot->key) {
            record->previous = slot->value + 1;
            const uint32_t earlier = registry->records[slot->value]->earliest;
            record->earliest = earlier < record->start ? earlier : record->start;
            slot->key = record->key;
        } else {
            markdown_core_key_index_commit(&registry->index, slot, record->key);
        }
        slot->value = space->cursor;
        space->indexed++;
    }
    space->cursor = registry->count;
    return true;
}

static const markdown_core_registry_record *S_find_key(markdown_core_registry *registry,
                                                       markdown_core_registry_kind kind, bool *failed) {
    if (registry->key.oom || !S_index_space(registry, kind)) {
        *failed = true;
        return NULL;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&registry->index, registry->key.ptr, registry->key.size);
    return slot ? registry->records[slot->value] : NULL;
}

const markdown_core_registry_record *markdown_core_registry_find(markdown_core_registry *registry,
                                                                 markdown_core_registry_kind kind,
                                                                 const unsigned char *label, bufsize_t length,
                                                                 bool *failed) {
    if (length <= 0 || !registry->spaces[kind].records) {
        return NULL;
    }
    markdown_core_strbuf_clear(&registry->key);
    markdown_core_strbuf_putc(&registry->key, (int)kind);
    markdown_core_strbuf_put(&registry->key, label, length);
    return S_find_key(registry, kind, failed);
}

const markdown_core_registry_record *markdown_core_registry_find_label(markdown_core_registry *registry,
                                                                       markdown_core_registry_kind kind,
                                                                       const markdown_core_chunk *label, bool *failed) {
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !registry->spaces[kind].records) {
        return NULL;
    }
    markdown_core_strbuf_clear(&registry->key);
    markdown_core_strbuf_putc(&registry->key, (int)kind);
    markdown_core_utf8proc_normalize_label(&registry->key, label->data, label->len);
    if (registry->key.size == 1 && !registry->key.oom) {
        return NULL;
    }
    return S_find_key(registry, kind, failed);
}

bool markdown_core_registry_normal_label(markdown_core_registry *registry, markdown_core_node_pool *pool,
                                         const markdown_core_chunk *label, markdown_core_chunk *normal) {
    *normal = (markdown_core_chunk){0};
    markdown_core_strbuf_clear(&registry->key);
    if (label->len > 0) {
        markdown_core_utf8proc_normalize_label(&registry->key, label->data, label->len);
    }
    if (registry->key.oom) {
        return false;
    }
    const bufsize_t length = registry->key.size;
    if (!length) {
        return true;
    }
    unsigned char *stored = markdown_core_node_pool_bytes(pool, (size_t)length + 1);
    if (!stored) {
        return false;
    }
    memcpy(stored, registry->key.ptr, (size_t)length);
    stored[length] = '\0';
    *normal = (markdown_core_chunk){stored, length, 0};
    return true;
}

markdown_core_registry_mark markdown_core_registry_mark_now(const markdown_core_registry *registry) {
    return (markdown_core_registry_mark){registry->count, registry->blocks, registry->block_used, registry->block_size};
}

void markdown_core_registry_rollback(markdown_core_registry *registry, markdown_core_registry_mark mark) {
    assert(mark.count <= registry->count);
    while (registry->count > mark.count) {
        const markdown_core_registry_record *record = registry->records[--registry->count];
        markdown_core_registry_space *space = &registry->spaces[record->key[0]];
        space->records--;
        /* A record the index never took leaves only the log. */
        if (registry->count < space->cursor) {
            space->indexed--;
            markdown_core_key_index_slot *slot =
                markdown_core_key_index_find(&registry->index, record->key, record->key_len);
            assert(slot && slot->value == registry->count);
            if (record->previous) {
                slot->value = record->previous - 1;
                slot->key = registry->records[slot->value]->key;
            } else {
                markdown_core_key_index_remove(&registry->index, slot);
            }
        }
    }
    for (size_t kind = 0; kind < sizeof(registry->spaces) / sizeof(*registry->spaces); kind++) {
        if (registry->spaces[kind].cursor > registry->count) {
            registry->spaces[kind].cursor = registry->count;
        }
    }
    while (registry->blocks != mark.blocks) {
        markdown_core_registry_block *block = registry->blocks;
        registry->blocks = block->head.next;
        markdown_core_free(block);
    }
    registry->block_used = mark.block_used;
    registry->block_size = mark.block_size;
}

bool markdown_core_reserve_anchor(markdown_core_reserved_anchors *anchors, markdown_core_node_pool *pool,
                                  const unsigned char *name, bufsize_t length) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&anchors->names, name, length);
    if (!slot) {
        return false;
    }
    if (!slot->key) {
        unsigned char *copy = markdown_core_node_pool_bytes(pool, (size_t)length);
        if (!copy) {
            return false;
        }
        memcpy(copy, name, (size_t)length);
        markdown_core_key_index_commit(&anchors->names, slot, copy);
        slot->value = 0;
    }
    slot->value++;
    return true;
}

void markdown_core_withdraw_anchor(markdown_core_reserved_anchors *anchors, markdown_core_node_pool *pool,
                                   const unsigned char *name, bufsize_t length) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_find(&anchors->names, name, length);
    assert(slot && slot->value);
    if (--slot->value == 0) {
        unsigned char *copy = (unsigned char *)slot->key;
        markdown_core_key_index_remove(&anchors->names, slot);
        markdown_core_node_pool_bytes_free(pool, copy);
    }
}

void markdown_core_reserved_anchors_free(markdown_core_reserved_anchors *anchors, markdown_core_node_pool *pool) {
    for (size_t i = 0; i < anchors->names.capacity; i++) {
        if (anchors->names.slots[i].key) {
            markdown_core_node_pool_bytes_free(pool, (unsigned char *)anchors->names.slots[i].key);
        }
    }
    markdown_core_key_index_free(&anchors->names);
}
