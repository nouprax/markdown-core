#include <assert.h>
#include <string.h>
#include "alloc.h"
#include "registry.h"
#include "parser.h"
#include "utf8.h"

/* THE LOG'S RECORDS are carved from a store the registry owns (carve.h):
 * pops come newest first, as a rollback makes them. */

void markdown_core_registry_init(markdown_core_registry *registry) {
    *registry = (markdown_core_registry){0};
    markdown_core_strbuf_init(&registry->key, 0);
}

void markdown_core_registry_free(markdown_core_registry *registry) {
    markdown_core_carve_free(&registry->carve);
    markdown_core_free(registry->records);
    markdown_core_ahead_free(&registry->records_ahead, NULL);
    markdown_core_key_index_free(&registry->index);
    markdown_core_strbuf_free(&registry->key);
    *registry = (markdown_core_registry){0};
    markdown_core_strbuf_init(&registry->key, 0);
}

static inline bool S_append(markdown_core_registry *registry, markdown_core_registry_kind kind,
                            const unsigned char *label, bufsize_t length, uint32_t start, bool withdraws) {
    if (registry->count == registry->room) {
        void *records = markdown_core_ahead_extend(&registry->records_ahead, NULL, registry->records, &registry->room,
                                                   registry->count, registry->count + 1, sizeof(*registry->records));
        if (!records) {
            return false;
        }
        registry->records = records;
    }
    markdown_core_registry_record *record =
        markdown_core_carve_take(&registry->carve, sizeof(*record) + (size_t)length + 2);
    if (!record) {
        return false;
    }
    record->key[0] = (unsigned char)kind;
    memcpy(record->key + 1, label, (size_t)length);
    record->key[length + 1] = '\0';
    record->key_len = length + 1;
    record->start = record->earliest = start;
    record->previous = 0;
    record->live = 0;
    record->withdraws = withdraws;
    registry->records[registry->count++] = record;
    registry->spaces[kind].records++;
    return true;
}

bool markdown_core_registry_declare(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                    const unsigned char *label, bufsize_t length, uint32_t start) {
    return length <= 0 || S_append(registry, kind, label, length, start, false);
}

/* The index takes the kind's records the log holds beyond its cursor, in log
 * order, with room asked for all of them first. */
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
            const markdown_core_registry_record *before = registry->records[slot->value];
            record->previous = slot->value + 1;
            if (record->withdraws) {
                assert(before->live);
                record->earliest = before->earliest;
                record->live = before->live - 1;
            } else {
                record->earliest = before->earliest < record->start ? before->earliest : record->start;
                record->live = before->live + 1;
            }
            slot->key = record->key;
        } else {
            assert(!record->withdraws);
            record->live = 1;
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
    return slot && registry->records[slot->value]->live ? registry->records[slot->value] : NULL;
}

bool markdown_core_registry_withdraw(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                     const unsigned char *label, bufsize_t length, uint32_t start) {
    assert(length > 0);
    return S_append(registry, kind, label, length, start, true) && S_index_space(registry, kind);
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
    return (markdown_core_registry_mark){registry->count, registry->carve};
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
    markdown_core_ahead_unsettle(&registry->records_ahead, registry->count);
    for (size_t kind = 0; kind < sizeof(registry->spaces) / sizeof(*registry->spaces); kind++) {
        if (registry->spaces[kind].cursor > registry->count) {
            registry->spaces[kind].cursor = registry->count;
        }
    }
    markdown_core_carve_rollback(&registry->carve, mark.carve);
}
