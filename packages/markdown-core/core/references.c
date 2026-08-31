#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

int markdown_core_association_init(
    markdown_core_mem *mem,
    markdown_core_association *out,
    const markdown_core_chunk *label,
    markdown_core_map_key *key
) {
    out->label.data = NULL;
    out->label.len = 0;
    out->label.alloc = 0;
    out->label.owner = NULL;
    out->identifier = out->label;

    /* The label OWNS its bytes: it is read out of a block's content buffer,
     * which a harvest may drop, and it outlives the parse. */
    out->label = markdown_core_chunk_dup(label, 0, label->len);
    if (!markdown_core_chunk_to_cstr(mem, &out->label)) {
        /* Do not keep borrowing the caller's bytes: a failed init can leave
         * the association on a node that outlives them, and a later reader --
         * the AST derivation was the witness -- walks into the freed buffer.
         * `chunk_free` on a borrow only clears the fields. */
        markdown_core_chunk_free(mem, &out->label);
        markdown_core_map_key_free(mem, key);
        return 0;
    }

    /* THE ADOPTION (#125): the identifier IS the prepared key's bytes -- the
     * same allocation the lookup probed with, moved, not re-derived. The key
     * is consumed on both outcomes, so ownership never forks. */
    out->identifier.data = key->bytes;
    out->identifier.len = key->len;
    out->identifier.alloc = 1;
    key->bytes = NULL;
    key->len = 0;
    return 1;
}

void markdown_core_association_free(markdown_core_mem *mem, markdown_core_association *association) {
    markdown_core_chunk_free(mem, &association->label);
    markdown_core_chunk_free(mem, &association->identifier);
}

/* One free function for both sets: a record is a label and nothing else. */
static void definition_free(markdown_core_map *map, markdown_core_map_record *record) {
    if (record != NULL) {
        map->mem->free(record->label);
        map->mem->free(record);
    }
}

/* The identifier arrives ALREADY NORMALIZED (#125): it is the association's
 * identifier, built by the one key construction, so registration copies
 * bytes and derives nothing. The record still owns its copy -- a map and
 * the node whose association fed it have independent lifetimes. */
static void definition_create(markdown_core_map *map, const markdown_core_chunk *identifier, uint32_t definition) {
    markdown_core_map_record *record;
    unsigned char *reflabel;

    /* The parser tolerates a missing map (map_new failure under a
     * NULL-returning allocator); definitions are then dropped. */
    if (map == NULL) {
        return;
    }

    /* An association that failed to construct has no identifier; there is
     * nothing to define. */
    if (identifier->data == NULL || identifier->len == 0) {
        return;
    }

    reflabel = (unsigned char *)map->mem->calloc((size_t)identifier->len + 1, 1);
    if (!reflabel) {
        map->oom = 1;
        return;
    }
    memcpy(reflabel, identifier->data, (size_t)identifier->len);

    record = (markdown_core_map_record *)map->mem->calloc(1, sizeof(*record));
    if (!record) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    record->label = reflabel;
    record->label_len = identifier->len;
    /* The registering definition block's identity (D4). Mints are monotone in
     * document order, so the value itself says which of two definitions of one
     * label came first; the preparation folds duplicates to the smallest, and
     * nothing here picks a winner. */
    record->definition = definition;
    record->next = map->refs;

    map->refs = record;
    map->size++;
    /* Every insert advances the generation, a duplicate label included: the
     * projection cache keys on it (T4), and a spurious invalidation is a
     * slow feed where a missed one would be a wrong tree. */
    map->generation++;
    /* A definition may arrive after a lookup has prepared the map: every
     * mid-stream projection interleaves the two (§12.4). A prepared HASH
     * index absorbs the late arrival in place (#124) -- one upsert, the
     * smallest identity keeping the slot -- where reopening the preparation
     * used to rebuild the whole index from scratch on the next lookup, an
     * O(feeds x definitions) shape under block-unit streaming. The sorted
     * representation stays rebuild-on-next-lookup: it is the degraded path,
     * and its cost model is the one it always had. A failed upsert (the one
     * growth refused) degrades the same way -- the record is in the list
     * either way, so nothing is lost and the rebuild can still succeed
     * later; first-wins survives every path because the identity beside the
     * label is stamped from a count no preparation rewrites. */
    if (map->prepared && map->indexed) {
        markdown_core_key_index_slot *slot =
            markdown_core_key_index_upsert(&map->index, record->label, record->label_len);
        if (slot) {
            if (slot->value == NULL || ((markdown_core_map_record *)slot->value)->definition > record->definition) {
                slot->value = record;
            }
        } else {
            map->prepared = 0;
        }
    } else {
        map->prepared = 0;
    }
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, definition_free);
}

void markdown_core_reference_create(
    markdown_core_map *map,
    const markdown_core_chunk *identifier,
    uint32_t definition
) {
    definition_create(map, identifier, definition);
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, definition_free);
}

void markdown_core_footnote_definition_create(
    markdown_core_map *map,
    const markdown_core_chunk *identifier,
    uint32_t definition
) {
    definition_create(map, identifier, definition);
}
