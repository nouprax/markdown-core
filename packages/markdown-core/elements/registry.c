#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "ast_internal.h"
#include "registry.h"

static markdown_core_registries *S_registries(const markdown_core_parser *parser) {
    return parser->revision->registries;
}

static void S_fail(markdown_core_parser *parser) {
    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
}

/* FACT LISTS. An entry's lists are in their registry's order. */
static bool S_list_reserve(markdown_core_fact_list *list, size_t count) {
    if (count <= list->capacity) {
        return true;
    }
    void *values = markdown_core_reserve(list->values, &list->capacity, count, sizeof(*list->values));
    if (!values) {
        return false;
    }
    list->values = values;
    return true;
}

static bool S_list_push(markdown_core_fact_list *list, markdown_core_fact *fact) {
    if (!S_list_reserve(list, list->count + 1)) {
        return false;
    }
    list->values[list->count++] = fact;
    return true;
}

/* Where a fact of `order` is, or would go, in `list`. */
static size_t S_list_seek(const markdown_core_fact_list *list, uint64_t order) {
    size_t low = 0, high = list->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (list->values[middle]->order < order) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* Puts `fact` in `list`, which has room for it. */
static void S_list_insert(markdown_core_fact_list *list, markdown_core_fact *fact) {
    size_t at = S_list_seek(list, fact->order);
    memmove(list->values + at + 1, list->values + at, (list->count - at) * sizeof(*list->values));
    list->values[at] = fact;
    list->count++;
}

static void S_list_remove(markdown_core_fact_list *list, const markdown_core_fact *fact) {
    size_t at = S_list_seek(list, fact->order);
    memmove(list->values + at, list->values + at + 1, (list->count - at - 1) * sizeof(*list->values));
    list->count--;
}

static uint64_t S_fact_position(const void *entry) { return (*(markdown_core_fact *const *)entry)->position; }

/* `list` in the order of its facts' positions, keeping the order of facts
 * at one position. */
static bool S_list_sort(markdown_core_parser *parser, markdown_core_fact_list *list) {
    return markdown_core_order_source_entries(&parser->source_order, list->values, list->count, sizeof(*list->values),
                                              S_fact_position);
}

/* THE INDEX. */
static markdown_core_key_group S_group(const markdown_core_registry_entry *entry) {
    return (markdown_core_key_group)entry->key[0];
}

/* Which of `entry`'s lists holds `fact`. */
static int S_list_of(const markdown_core_registry_entry *entry, const markdown_core_fact *fact) {
    switch (S_group(entry)) {
    case MARKDOWN_CORE_KEY_REFERENCE:
        return fact->kind == MARKDOWN_CORE_FACT_HEADING;
    case MARKDOWN_CORE_KEY_FAMILY:
        return fact->kind == MARKDOWN_CORE_FACT_ANCHOR;
    default:
        return 0;
    }
}

/* Puts `entry` on the round's list of touched entries, its round state
 * cleared, unless it is there. */
static void S_touch(markdown_core_registries *registries, markdown_core_registry_entry *entry) {
    if (entry->round == registries->round) {
        return;
    }
    entry->round = registries->round;
    entry->touched = registries->touched;
    registries->touched = entry;
    entry->moved = entry->changed = entry->answered = false;
    entry->fresh.count = 0;
    entry->answer = NULL;
}

/* The entry of `label` in `group`, made when there is none, or NULL when
 * storage ran out. A new entry is touched, so a round that does not commit
 * removes it. */
static markdown_core_registry_entry *S_entry(markdown_core_registries *registries, markdown_core_key_group group,
                                             const unsigned char *label, bufsize_t length) {
    markdown_core_strbuf *key = &registries->key;
    markdown_core_strbuf_clear(key);
    markdown_core_strbuf_putc(key, (int)group);
    markdown_core_strbuf_put(key, label, length);
    if (key->oom) {
        return NULL;
    }
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&registries->index, key->ptr, key->size);
    if (!slot) {
        return NULL;
    }
    if (slot->key) {
        return slot->value.pointer;
    }
    markdown_core_registry_entry *entry = markdown_core_alloc(1, sizeof(*entry));
    unsigned char *bytes = entry ? markdown_core_alloc((size_t)key->size + 1, 1) : NULL;
    if (!bytes) {
        markdown_core_free(entry);
        /* The vacant slot is committed only with a key; an uncommitted one
         * holds nothing the index reads. */
        slot->hash = 0;
        slot->key_len = 0;
        return NULL;
    }
    memcpy(bytes, key->ptr, (size_t)key->size);
    entry->key = bytes;
    entry->key_length = key->size;
    markdown_core_key_index_commit(&registries->index, slot, bytes);
    slot->value.pointer = entry;
    S_touch(registries, entry);
    return entry;
}

static void S_entry_free(markdown_core_registry_entry *entry) {
    markdown_core_free(entry->lists[0].values);
    markdown_core_free(entry->lists[1].values);
    markdown_core_free(entry->fresh.values);
    markdown_core_free(entry->key);
    markdown_core_free(entry);
}

/* Removes the touched entries that list nothing. */
static void S_sweep(markdown_core_registries *registries) {
    for (markdown_core_registry_entry *entry = registries->touched, *next; entry; entry = next) {
        next = entry->touched;
        if (!entry->lists[0].count && !entry->lists[1].count && !entry->lookups) {
            markdown_core_key_index_remove(&registries->index, entry->key, entry->key_length);
            S_entry_free(entry);
        }
    }
    registries->touched = NULL;
}

/* The label in `registries->label` in its normal form: false, failing the
 * parse when storage ran out, when it has none. */
static bool S_normalize(markdown_core_parser *parser, markdown_core_registries *registries,
                        markdown_core_chunk *label) {
    if (normalize_map_label_into(&registries->label, label)) {
        return true;
    }
    if (registries->label.oom) {
        S_fail(parser);
    }
    return false;
}

/* The anchor family of `spelling`: it less every trailing `-N` group. */
static bufsize_t S_stem(const unsigned char *spelling, bufsize_t length) {
    for (;;) {
        bufsize_t at = length;
        while (at > 0 && spelling[at - 1] >= '0' && spelling[at - 1] <= '9') {
            at--;
        }
        if (at == length || at == 0 || spelling[at - 1] != '-') {
            return length;
        }
        length = at - 1;
    }
}

void markdown_core_registries_init(markdown_core_registries *registries) {
    memset(registries, 0, sizeof(*registries));
    markdown_core_strbuf_init(&registries->label, 0);
    markdown_core_strbuf_init(&registries->key, 0);
}

void markdown_core_registries_dispose(markdown_core_registries *registries, markdown_core_node_pool *pool) {
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        if (registries->registries[kind]) {
            markdown_core_node_pool_release(pool, registries->registries[kind]);
        }
        markdown_core_free(registries->fresh[kind].values);
        markdown_core_free(registries->dropped[kind].values);
    }
    for (int i = 0; i < 2; i++) {
        if (registries->labels[i]) {
            markdown_core_node_pool_release(pool, registries->labels[i]);
        }
    }
    for (size_t i = 0; i < registries->index.capacity; i++) {
        if (registries->index.slots[i].key) {
            S_entry_free(registries->index.slots[i].value.pointer);
        }
    }
    markdown_core_key_index_free(&registries->index);
    markdown_core_free(registries->touches);
    markdown_core_strbuf_free(&registries->label);
    markdown_core_strbuf_free(&registries->key);
    memset(registries, 0, sizeof(*registries));
}

void markdown_core_registries_begin(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    registries->round++;
    registries->touched = NULL;
    registries->touch_count = 0;
}

static void S_unstage(markdown_core_node_pool *pool, markdown_core_registries *registries);

void markdown_core_registries_end(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    S_unstage(parser->pool, registries);
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        markdown_core_fact_list *fresh = &registries->fresh[kind];
        for (size_t i = 0; i < fresh->count; i++) {
            markdown_core_fact_release(parser->pool, fresh->values[i]);
        }
        fresh->count = 0;
        registries->dropped[kind].count = 0;
    }
    S_sweep(registries);
}

/* THE ROUND'S VIEW OF OLD FACTS. An old fact stays when a node the parse
 * took holds it: when it is inside a range the parse took (parser.h). */
static int64_t S_position(const markdown_core_registries *registries, const markdown_core_fact *fact) {
    const markdown_core_node *registry = registries->registries[fact->kind];
    return markdown_core_registry_position(registry, markdown_core_registry_rank(registry, fact));
}

static bool S_taken(const markdown_core_parser *parser, int64_t position) {
    size_t low = 0, high = parser->take_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if ((int64_t)parser->takes[middle].start <= position) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low && position < (int64_t)parser->takes[low - 1].end;
}

/* The first fact of `list` that stays, and where it is; NULL for none. */
static markdown_core_fact *S_first_staying(const markdown_core_parser *parser,
                                           const markdown_core_registries *registries,
                                           const markdown_core_fact_list *list, int64_t *position) {
    for (size_t i = 0; i < list->count; i++) {
        *position = S_position(registries, list->values[i]);
        if (S_taken(parser, *position)) {
            return list->values[i];
        }
    }
    return NULL;
}

static void S_touch_place(markdown_core_parser *parser, markdown_core_registries *registries, int64_t position) {
    void *grown = markdown_core_reserve(registries->touches, &registries->touch_capacity, registries->touch_count + 1,
                                        sizeof(*registries->touches));
    if (!grown) {
        S_fail(parser);
        return;
    }
    registries->touches = grown;
    registries->touches[registries->touch_count++] = (uint32_t)position;
}

/* DECLARATIONS. */
static markdown_core_fact *S_declare(markdown_core_parser *parser, markdown_core_fact_kind kind, uint32_t position) {
    markdown_core_registries *registries = S_registries(parser);
    markdown_core_fact *fact = markdown_core_fact_new(kind);
    if (!fact || !S_list_push(&registries->fresh[kind], fact)) {
        if (fact) {
            markdown_core_fact_release(parser->pool, fact);
        }
        S_fail(parser);
        return NULL;
    }
    fact->position = position;
    return fact;
}

/* `fact`, fresh, declares the key of `entry`. */
static void S_declare_in(markdown_core_parser *parser, markdown_core_registry_entry *entry, int slot,
                         markdown_core_fact *fact) {
    markdown_core_registries *registries = S_registries(parser);
    if (!entry) {
        S_fail(parser);
        return;
    }
    fact->entries[slot] = entry;
    S_touch(registries, entry);
    entry->moved = true;
    if (!S_list_push(&entry->fresh, fact)) {
        S_fail(parser);
    }
}

void markdown_core_registries_declare_reference(markdown_core_parser *parser, markdown_core_chunk *label,
                                                markdown_core_resource *resource, uint32_t position) {
    markdown_core_registries *registries = S_registries(parser);
    markdown_core_fact *fact =
        S_normalize(parser, registries, label) ? S_declare(parser, MARKDOWN_CORE_FACT_REFERENCE, position) : NULL;
    if (!fact) {
        markdown_core_resource_release(&parser->pool->resources, resource);
        return;
    }
    fact->resource = resource;
    S_declare_in(parser,
                 S_entry(registries, MARKDOWN_CORE_KEY_REFERENCE, registries->label.ptr, registries->label.size), 0,
                 fact);
}

markdown_core_fact *markdown_core_registries_declare_heading(markdown_core_parser *parser, markdown_core_chunk *label,
                                                             markdown_core_resource *resource, uint32_t position) {
    markdown_core_registries *registries = S_registries(parser);
    if (resource && !S_normalize(parser, registries, label)) {
        markdown_core_resource_release(&parser->pool->resources, resource);
        resource = NULL;
    }
    markdown_core_fact *fact = parser->error ? NULL : S_declare(parser, MARKDOWN_CORE_FACT_HEADING, position);
    if (!fact) {
        markdown_core_resource_release(&parser->pool->resources, resource);
        return NULL;
    }
    fact->resource = resource;
    if (resource) {
        S_declare_in(parser,
                     S_entry(registries, MARKDOWN_CORE_KEY_REFERENCE, registries->label.ptr, registries->label.size), 0,
                     fact);
    }
    return fact;
}

void markdown_core_registries_heading_base(markdown_core_parser *parser, markdown_core_fact *heading,
                                           const unsigned char *base, bufsize_t length) {
    unsigned char *bytes = markdown_core_alloc((size_t)length + 1, 1);
    if (!bytes) {
        S_fail(parser);
        return;
    }
    memcpy(bytes, base, (size_t)length);
    heading->base = markdown_core_optional_chunk_present((markdown_core_chunk){bytes, length, 1});
    S_declare_in(parser, S_entry(S_registries(parser), MARKDOWN_CORE_KEY_FAMILY, bytes, S_stem(bytes, length)), 1,
                 heading);
}

void markdown_core_registries_declare_definition(markdown_core_parser *parser, markdown_core_node *node,
                                                 uint32_t position) {
    bool footnote = node->kind == MARKDOWN_CORE_NODE_FOOTNOTE;
    markdown_core_fact *fact =
        S_declare(parser, footnote ? MARKDOWN_CORE_FACT_FOOTNOTE : MARKDOWN_CORE_FACT_SPECIMEN, position);
    if (!fact) {
        return;
    }
    fact->node = markdown_core_node_retain(node);
    const markdown_core_optional_chunk *label = footnote ? &node->as.footnote->label : &node->as.specimen->label;
    if (label->has_value) {
        S_declare_in(parser,
                     S_entry(S_registries(parser), footnote ? MARKDOWN_CORE_KEY_FOOTNOTE : MARKDOWN_CORE_KEY_SPECIMEN,
                             label->value.data, label->value.len),
                     0, fact);
    }
}

void markdown_core_registries_declare_anchor(markdown_core_parser *parser, const markdown_core_chunk *anchor,
                                             uint32_t position) {
    markdown_core_fact *fact = S_declare(parser, MARKDOWN_CORE_FACT_ANCHOR, position);
    unsigned char *bytes = fact ? markdown_core_alloc((size_t)anchor->len + 1, 1) : NULL;
    if (!bytes) {
        if (fact) {
            S_fail(parser);
        }
        return;
    }
    memcpy(bytes, anchor->data, (size_t)anchor->len);
    fact->anchor = (markdown_core_chunk){bytes, anchor->len, 1};
    S_declare_in(parser, S_entry(S_registries(parser), MARKDOWN_CORE_KEY_FAMILY, bytes, S_stem(bytes, anchor->len)), 0,
                 fact);
}

void markdown_core_registries_seal(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS && !parser->error; kind++) {
        const markdown_core_node *registry = registries->registries[kind];
        if (!registry) {
            continue;
        }
        /* The gaps between the ranges the parse took are what it read. */
        int64_t from = 0;
        for (size_t take = 0;; take++) {
            size_t first = markdown_core_registry_seek(registry, from);
            size_t last = take < parser->take_count ? markdown_core_registry_seek(registry, parser->takes[take].start)
                                                    : markdown_core_registry_count(registry);
            markdown_core_children_cursor cursor;
            markdown_core_children_seek(&cursor, registry->children, first);
            for (size_t i = first; i < last; i++) {
                markdown_core_fact *fact = markdown_core_fact_of(markdown_core_children_next(&cursor));
                if (!S_list_push(&registries->dropped[kind], fact)) {
                    S_fail(parser);
                    return;
                }
                for (int slot = 0; slot < 2; slot++) {
                    markdown_core_registry_entry *entry = fact->entries[slot];
                    if (entry) {
                        S_touch(registries, entry);
                        entry->moved = entry->moved || kind != MARKDOWN_CORE_FACT_LOOKUP;
                    }
                }
            }
            if (take == parser->take_count) {
                break;
            }
            from = parser->takes[take].end;
        }
    }
}

/* LOOKUPS. */

/* The fact `entry`'s key resolves to: the first of its facts, its first
 * list before its second; in a round that changed them, the first by
 * position of the old ones that stay and the fresh ones. */
static markdown_core_fact *S_answer(const markdown_core_parser *parser, markdown_core_registries *registries,
                                    markdown_core_registry_entry *entry) {
    if (entry->round != registries->round || !entry->moved) {
        return entry->lists[0].count   ? entry->lists[0].values[0]
               : entry->lists[1].count ? entry->lists[1].values[0]
                                       : NULL;
    }
    if (entry->answered) {
        return entry->answer;
    }
    markdown_core_fact *answer = NULL;
    for (int list = 0; list < 2 && !answer; list++) {
        int64_t position = 0;
        answer = S_first_staying(parser, registries, &entry->lists[list], &position);
        for (size_t i = 0; i < entry->fresh.count; i++) {
            markdown_core_fact *fact = entry->fresh.values[i];
            if (S_list_of(entry, fact) == list && (!answer || fact->position < position)) {
                answer = fact;
                position = fact->position;
            }
        }
    }
    entry->answer = answer;
    entry->answered = true;
    return answer;
}

/* The entry of the key a lookup asks for, the lookup recorded; NULL when
 * the parse failed. */
static markdown_core_registry_entry *S_lookup(markdown_core_parser *parser, markdown_core_key_group group,
                                              const unsigned char *label, bufsize_t length) {
    markdown_core_registries *registries = S_registries(parser);
    markdown_core_registry_entry *entry = S_entry(registries, group, label, length);
    markdown_core_fact *fact = entry ? S_declare(parser, MARKDOWN_CORE_FACT_LOOKUP, parser->lookup_at) : NULL;
    if (!fact) {
        S_fail(parser);
        return NULL;
    }
    fact->entries[0] = entry;
    return entry;
}

markdown_core_resource *markdown_core_registries_reference(markdown_core_parser *parser, markdown_core_chunk *label) {
    markdown_core_registries *registries = S_registries(parser);
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !S_normalize(parser, registries, label)) {
        return NULL;
    }
    markdown_core_registry_entry *entry =
        S_lookup(parser, MARKDOWN_CORE_KEY_REFERENCE, registries->label.ptr, registries->label.size);
    markdown_core_fact *answer = entry ? S_answer(parser, registries, entry) : NULL;
    return answer ? answer->resource : NULL;
}

bool markdown_core_registries_footnote(markdown_core_parser *parser, markdown_core_chunk *label,
                                       markdown_core_chunk *normal) {
    markdown_core_registries *registries = S_registries(parser);
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !S_normalize(parser, registries, label)) {
        return false;
    }
    markdown_core_registry_entry *entry =
        S_lookup(parser, MARKDOWN_CORE_KEY_FOOTNOTE, registries->label.ptr, registries->label.size);
    if (!entry || !S_answer(parser, registries, entry)) {
        return false;
    }
    *normal = (markdown_core_chunk){entry->key + 1, entry->key_length - 1, 0};
    return true;
}

bool markdown_core_registries_specimen(markdown_core_parser *parser, const markdown_core_chunk *id) {
    markdown_core_registry_entry *entry = S_lookup(parser, MARKDOWN_CORE_KEY_SPECIMEN, id->data, id->len);
    return entry && S_answer(parser, S_registries(parser), entry);
}

/* RESOLUTION. */

/* A heading of a family, and where it is. */
typedef struct {
    markdown_core_fact *fact;
    int64_t position;
} family_heading;

static uint64_t S_family_position(const void *entry) { return (uint64_t)((const family_heading *)entry)->position; }

/* The suffix `-ordinal` after `base`. */
static void S_append_suffix(markdown_core_strbuf *base, size_t ordinal) {
    char suffix[3 * sizeof(size_t) + 1];
    char *end = suffix + sizeof(suffix), *start = end;
    do {
        *--start = (char)('0' + ordinal % 10);
        ordinal /= 10;
    } while (ordinal);
    *--start = '-';
    markdown_core_strbuf_put(base, (const unsigned char *)start, (bufsize_t)(end - start));
}

/* ASSIGNS ONE FAMILY, as a parse of the whole document assigns it: its
 * explicit anchors are reserved, and each of its headings, in source
 * order, takes its base, or the base with the first `-N` suffix after the
 * last one the base took that nothing holds. A fresh heading takes the
 * anchor; a heading that stays and would take another one than it holds is
 * touched, and so are the lookups of its label. */
static void S_assign_family(markdown_core_parser *parser, markdown_core_registries *registries,
                            markdown_core_registry_entry *entry) {
    markdown_core_key_index taken = {0};
    family_heading *headings = NULL;
    size_t heading_count = 0, heading_capacity = 0;
    /* Anchors this assignment spelled that no fact keeps, freed with the
     * index that borrows them. */
    unsigned char **spelled = NULL;
    size_t spelled_count = 0, spelled_capacity = 0;
    markdown_core_strbuf base = MARKDOWN_CORE_BUF_INIT();
    bool ok = markdown_core_key_index_init(&taken, entry->lists[0].count + entry->lists[1].count + entry->fresh.count);
    /* The reservations, and the headings with where they are. */
    for (size_t i = 0; ok && i < entry->lists[1].count + entry->fresh.count; i++) {
        markdown_core_fact *fact = NULL;
        if (i < entry->lists[1].count) {
            fact = entry->lists[1].values[i];
            if (!S_taken(parser, S_position(registries, fact))) {
                continue;
            }
        } else if ((fact = entry->fresh.values[i - entry->lists[1].count])->kind != MARKDOWN_CORE_FACT_ANCHOR) {
            continue;
        }
        markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&taken, fact->anchor.data, fact->anchor.len);
        if (!(ok = slot != NULL)) {
            break;
        }
        if (!slot->key) {
            markdown_core_key_index_commit(&taken, slot, fact->anchor.data);
            slot->value.counter = 1;
        }
    }
    for (size_t i = 0; ok && i < entry->lists[0].count + entry->fresh.count; i++) {
        family_heading heading;
        if (i < entry->lists[0].count) {
            heading.fact = entry->lists[0].values[i];
            heading.position = S_position(registries, heading.fact);
            if (!S_taken(parser, heading.position)) {
                continue;
            }
        } else if ((heading.fact = entry->fresh.values[i - entry->lists[0].count])->kind ==
                   MARKDOWN_CORE_FACT_HEADING) {
            heading.position = heading.fact->position;
        } else {
            continue;
        }
        void *grown = markdown_core_reserve(headings, &heading_capacity, heading_count + 1, sizeof(*headings));
        if (!(ok = grown != NULL)) {
            break;
        }
        headings = grown;
        headings[heading_count++] = heading;
    }
    ok = ok && markdown_core_order_source_entries(&parser->source_order, headings, heading_count, sizeof(*headings),
                                                  S_family_position);
    for (size_t i = 0; ok && i < heading_count; i++) {
        markdown_core_fact *heading = headings[i].fact;
        markdown_core_strbuf_clear(&base);
        markdown_core_strbuf_put(&base, heading->base.value.data, heading->base.value.len);
        bufsize_t base_length = base.size;
        markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&taken, base.ptr, base.size);
        markdown_core_key_index_slot *candidate = slot;
        if (slot && slot->key) {
            do {
                markdown_core_strbuf_truncate(&base, base_length);
                S_append_suffix(&base, slot->value.counter++);
                if (base.oom) {
                    candidate = NULL;
                    break;
                }
                /* Only a vacant candidate can grow the index, and the base's
                 * slot is not read after one. */
                candidate = markdown_core_key_index_entry(&taken, base.ptr, base.size);
            } while (candidate && candidate->key);
        }
        unsigned char *bytes = candidate && !base.oom ? markdown_core_alloc((size_t)base.size + 1, 1) : NULL;
        if (!(ok = bytes != NULL)) {
            break;
        }
        memcpy(bytes, base.ptr, (size_t)base.size);
        markdown_core_chunk anchor = {bytes, base.size, 1};
        if (!heading->order) {
            heading->anchor = anchor;
        } else if (heading->anchor.len == anchor.len && !memcmp(heading->anchor.data, bytes, (size_t)anchor.len)) {
            markdown_core_free(bytes);
            bytes = heading->anchor.data;
        } else {
            void *grown = markdown_core_reserve(spelled, &spelled_capacity, spelled_count + 1, sizeof(*spelled));
            if (!(ok = grown != NULL)) {
                markdown_core_free(bytes);
                break;
            }
            spelled = grown;
            spelled[spelled_count++] = bytes;
            S_touch_place(parser, registries, headings[i].position);
            if (heading->entries[0]) {
                heading->entries[0]->changed = true;
            }
        }
        markdown_core_key_index_commit(&taken, candidate, bytes);
        candidate->value.counter = 1;
    }
    if (!ok) {
        S_fail(parser);
    }
    for (size_t i = 0; i < spelled_count; i++) {
        markdown_core_free(spelled[i]);
    }
    markdown_core_free(spelled);
    markdown_core_free(headings);
    markdown_core_strbuf_free(&base);
    markdown_core_key_index_free(&taken);
}

void markdown_core_registries_assign_anchors(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    for (markdown_core_registry_entry *entry = registries->touched; entry && !parser->error; entry = entry->touched) {
        if (S_group(entry) == MARKDOWN_CORE_KEY_FAMILY && entry->moved) {
            S_assign_family(parser, registries, entry);
        }
    }
}

/* Whether the answers `old` and `now` of a key of `group` differ as values:
 * a reference's resources, or a definition's being there. */
static bool S_answers_differ(markdown_core_key_group group, const markdown_core_fact *old,
                             const markdown_core_fact *now) {
    if (!old || !now) {
        return old != now;
    }
    return group == MARKDOWN_CORE_KEY_REFERENCE && old != now &&
           !markdown_core_resource_equal(old->resource, now->resource);
}

static uint64_t S_touch_position(const void *entry) { return *(const uint32_t *)entry; }

void markdown_core_registries_resolve(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    for (markdown_core_registry_entry *entry = registries->touched; entry && !parser->error; entry = entry->touched) {
        markdown_core_key_group group = S_group(entry);
        if (group == MARKDOWN_CORE_KEY_FAMILY || !(entry->moved || entry->changed)) {
            continue;
        }
        const markdown_core_fact *old = entry->lists[0].count   ? entry->lists[0].values[0]
                                        : entry->lists[1].count ? entry->lists[1].values[0]
                                                                : NULL;
        if (!entry->changed && !S_answers_differ(group, old, S_answer(parser, registries, entry))) {
            continue;
        }
        for (const markdown_core_fact *lookup = entry->lookups; lookup; lookup = lookup->next) {
            int64_t position = S_position(registries, lookup);
            if (S_taken(parser, position)) {
                S_touch_place(parser, registries, position);
            }
        }
    }
    if (parser->error) {
        return;
    }
    if (registries->touch_count) {
        if (!markdown_core_order_source_entries(&parser->source_order, registries->touches, registries->touch_count,
                                                sizeof(*registries->touches), S_touch_position)) {
            S_fail(parser);
            return;
        }
        size_t count = 1;
        for (size_t i = 1; i < registries->touch_count; i++) {
            if (registries->touches[i] != registries->touches[count - 1]) {
                registries->touches[count++] = registries->touches[i];
            }
        }
        registries->touch_count = count;
        parser->revision->touches = registries->touches;
        parser->revision->touch_count = count;
        return;
    }
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        if (!S_list_sort(parser, &registries->fresh[kind])) {
            S_fail(parser);
            return;
        }
    }
}

void markdown_core_registries_remap(markdown_core_parser *parser, uint32_t start, const markdown_core_node *node) {
    markdown_core_fact_list *fresh =
        &S_registries(parser)->fresh[node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? MARKDOWN_CORE_FACT_FOOTNOTE
                                                                               : MARKDOWN_CORE_FACT_SPECIMEN];
    size_t low = 0, high = fresh->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (fresh->values[middle]->position < start) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    markdown_core_fact *fact = fresh->values[low];
    markdown_core_node_pool_release(parser->pool, fact->node);
    fact->node = markdown_core_node_retain((markdown_core_node *)node);
}

/* COMMIT. */

/* The registry of `kind` after the round: the runs of old facts inside the
 * ranges the parse took, and the fresh facts between them. NULL in `*built`
 * when the round changed nothing of the kind. False when storage ran out. */
static bool S_build(markdown_core_parser *parser, markdown_core_registries *registries, int kind,
                    markdown_core_registry_builder *builder, markdown_core_node **built) {
    const markdown_core_node *old = registries->registries[kind];
    const markdown_core_fact_list *fresh = &registries->fresh[kind];
    *built = NULL;
    if (!fresh->count && !registries->dropped[kind].count) {
        return true;
    }
    if (!markdown_core_registry_build_begin(builder, parser->pool)) {
        return false;
    }
    bool ok = true;
    size_t next = 0;
    for (size_t take = 0; ok && take <= parser->take_count; take++) {
        int64_t end = take < parser->take_count ? (int64_t)parser->takes[take].start : INT64_MAX;
        for (; ok && next < fresh->count && fresh->values[next]->position < end; next++) {
            ok = markdown_core_registry_build_put(builder, fresh->values[next], fresh->values[next]->position);
        }
        if (take == parser->take_count || !old) {
            continue;
        }
        size_t first = markdown_core_registry_seek(old, parser->takes[take].start);
        size_t last = markdown_core_registry_seek(old, parser->takes[take].end);
        if (ok && last > first) {
            ok = markdown_core_registry_build_join(builder, old, first, last - first,
                                                   markdown_core_registry_position(old, first));
        }
    }
    *built = markdown_core_registry_build_end(builder, ok);
    return *built != NULL;
}

/* The label of a footnote or specimen definition. */
static const markdown_core_optional_chunk *S_definition_label(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &node->as.footnote->label : &node->as.specimen->label;
}

/* A definition's label against the label of `entry`'s key, in byte order,
 * the shorter first on a common prefix. */
static int S_label_compare(const markdown_core_fact *fact, const markdown_core_registry_entry *entry) {
    const markdown_core_optional_chunk *label = S_definition_label(fact->node);
    size_t length = (size_t)label->value.len, key_length = (size_t)entry->key_length - 1;
    size_t common = length < key_length ? length : key_length;
    int order = common ? memcmp(label->value.data, entry->key + 1, common) : 0;
    return order ? order : (length > key_length) - (length < key_length);
}

static bool S_label_below(const markdown_core_fact *fact, const void *key) { return S_label_compare(fact, key) < 0; }

/* `table`, the definitions that win their labels, with the label of
 * `entry` won by `winner`, or by none. False when storage ran out. */
static bool S_label_update(markdown_core_node_pool *pool, markdown_core_node *table,
                           const markdown_core_registry_entry *entry, markdown_core_fact *winner) {
    size_t index = markdown_core_registry_search(table, S_label_below, entry);
    markdown_core_fact *held = index < markdown_core_registry_count(table)
                                   ? markdown_core_fact_of(markdown_core_children_at(table->children, index))
                                   : NULL;
    if (held && S_label_compare(held, entry)) {
        held = NULL;
    }
    if (held == winner) {
        return true;
    }
    markdown_core_node *removed = NULL;
    if (!winner) {
        if (!markdown_core_children_remove(pool, &table->children, index, &removed)) {
            return false;
        }
        markdown_core_node_pool_release(pool, removed);
        return true;
    }
    markdown_core_node *place = markdown_core_registry_place(pool, winner, 0);
    if (!place || !(held ? markdown_core_children_replace(pool, &table->children, index, place, &removed)
                         : markdown_core_children_insert(pool, &table->children, index, place))) {
        if (place) {
            markdown_core_node_pool_release(pool, place);
        }
        return false;
    }
    if (removed) {
        markdown_core_node_pool_release(pool, removed);
    }
    return true;
}

/* Gives back the registries and label tables a round staged and did not
 * commit. */
static void S_unstage(markdown_core_node_pool *pool, markdown_core_registries *registries) {
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        if (registries->built[kind]) {
            markdown_core_registry_build_cancel(&registries->builders[kind]);
            markdown_core_node_pool_release(pool, registries->built[kind]);
            registries->built[kind] = NULL;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (registries->built_labels[i]) {
            markdown_core_node_pool_release(pool, registries->built_labels[i]);
            registries->built_labels[i] = NULL;
        }
    }
}

bool markdown_core_registries_stage(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    markdown_core_node_pool *pool = parser->pool;
    markdown_core_registry_builder *builders = registries->builders;
    markdown_core_node **built = registries->built, **labels = registries->built_labels;
    bool ok = true;
    /* The registries, room in the entries' lists for their fresh facts, and
     * the label tables, built beside the ones the session holds. */
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        if (ok) {
            ok = S_build(parser, registries, kind, &builders[kind], &built[kind]);
        }
    }
    for (int i = 0; ok && i < 2; i++) {
        labels[i] = markdown_core_registry_new(pool);
        if (!(ok = labels[i] != NULL) || !registries->labels[i]) {
            continue;
        }
        labels[i]->children = markdown_core_run_retain(registries->labels[i]->children);
    }
    for (markdown_core_registry_entry *entry = registries->touched; ok && entry; entry = entry->touched) {
        ok = S_list_reserve(&entry->lists[0], entry->lists[0].count + entry->fresh.count) &&
             S_list_reserve(&entry->lists[1], entry->lists[1].count + entry->fresh.count);
        markdown_core_key_group group = S_group(entry);
        if (ok && entry->moved && (group == MARKDOWN_CORE_KEY_FOOTNOTE || group == MARKDOWN_CORE_KEY_SPECIMEN)) {
            ok = S_label_update(pool, labels[group == MARKDOWN_CORE_KEY_SPECIMEN], entry,
                                S_answer(parser, registries, entry));
        }
    }
    if (!ok) {
        S_unstage(pool, registries);
    }
    return ok;
}

void markdown_core_registries_commit(markdown_core_parser *parser) {
    markdown_core_registries *registries = S_registries(parser);
    markdown_core_node_pool *pool = parser->pool;
    markdown_core_registry_builder *builders = registries->builders;
    markdown_core_node **built = registries->built, **labels = registries->built_labels;
    /* Nothing here fails. The dropped facts leave the lists while their
     * orders still sort them, the registries take their new orders, and the
     * fresh facts join the lists by them. */
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        const markdown_core_fact_list *dropped = &registries->dropped[kind];
        for (size_t i = 0; i < dropped->count; i++) {
            markdown_core_fact *fact = dropped->values[i];
            if (kind == MARKDOWN_CORE_FACT_LOOKUP) {
                if (fact->previous) {
                    fact->previous->next = fact->next;
                } else {
                    fact->entries[0]->lookups = fact->next;
                }
                if (fact->next) {
                    fact->next->previous = fact->previous;
                }
                continue;
            }
            for (int slot = 0; slot < 2; slot++) {
                if (fact->entries[slot]) {
                    S_list_remove(&fact->entries[slot]->lists[S_list_of(fact->entries[slot], fact)], fact);
                }
            }
        }
    }
    for (int kind = 0; kind < MARKDOWN_CORE_FACT_KINDS; kind++) {
        if (!built[kind]) {
            continue;
        }
        markdown_core_registry_build_order(&builders[kind]);
        if (registries->registries[kind]) {
            markdown_core_node_pool_release(pool, registries->registries[kind]);
        }
        registries->registries[kind] = built[kind];
        built[kind] = NULL;
        const markdown_core_fact_list *fresh = &registries->fresh[kind];
        for (size_t i = 0; i < fresh->count; i++) {
            markdown_core_fact *fact = fresh->values[i];
            if (kind == MARKDOWN_CORE_FACT_LOOKUP) {
                markdown_core_registry_entry *entry = fact->entries[0];
                fact->previous = NULL;
                fact->next = entry->lookups;
                if (entry->lookups) {
                    entry->lookups->previous = fact;
                }
                entry->lookups = fact;
                continue;
            }
            for (int slot = 0; slot < 2; slot++) {
                if (fact->entries[slot]) {
                    S_list_insert(&fact->entries[slot]->lists[S_list_of(fact->entries[slot], fact)], fact);
                }
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (registries->labels[i]) {
            markdown_core_node_pool_release(pool, registries->labels[i]);
        }
        markdown_core_children_seal(labels[i]->children);
        registries->labels[i] = labels[i];
        labels[i] = NULL;
    }
}
