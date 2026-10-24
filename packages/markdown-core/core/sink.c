#include "alloc.h"
#include "sink.h"

static uint64_t mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 33);
}

static uint64_t opener_hash(const markdown_core_context_opener *opener) {
    return mix(((uint64_t)opener->place.start << 32 | opener->place.end) ^
               mix((uint64_t)opener->kind << 1 | opener->block));
}

static uint64_t field_hash(const markdown_core_context_field *field) {
    return mix(((uint64_t)field->start << 32 | (uint32_t)field->opener) ^
               mix((uint64_t)field->kind << 32 | (uint64_t)field->owner << 1 | field->declares));
}

static bool same_opener(const markdown_core_context_opener *a, const markdown_core_context_opener *b) {
    return a->kind == b->kind && a->place.start == b->place.start && a->place.end == b->place.end &&
           a->block == b->block;
}

static bool same_field(const markdown_core_context_field *a, const markdown_core_context_field *b) {
    return a->kind == b->kind && a->owner == b->owner && a->opener == b->opener && a->start == b->start &&
           a->declares == b->declares;
}

/* Rebuilds the slots of `count` listed entries at twice their count, at
 * least 16, so at most half the slots are full; false when they could not
 * be allocated. `hash` gives an entry's hash by its index. */
static bool rehash(uint32_t **slots, size_t *slot_count, size_t count, const void *entries, size_t width,
                   uint64_t (*hash)(const void *)) {
    size_t grown = 16;
    while (grown < 2 * (count + 1)) {
        grown *= 2;
    }
    uint32_t *fresh = markdown_core_alloc(grown, sizeof(*fresh));
    if (!fresh) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        size_t at = (size_t)hash((const char *)entries + i * width) & (grown - 1);
        while (fresh[at]) {
            at = (at + 1) & (grown - 1);
        }
        fresh[at] = (uint32_t)i + 1;
    }
    markdown_core_free(*slots);
    *slots = fresh;
    *slot_count = grown;
    return true;
}

static uint64_t any_opener_hash(const void *opener) { return opener_hash(opener); }
static uint64_t any_field_hash(const void *field) { return field_hash(field); }

int32_t markdown_core_context_opener_add(markdown_core_context_sink *sink, markdown_core_node_kind kind,
                                         markdown_core_place place, bool block) {
    const markdown_core_context_opener opener = {kind, place, block};
    if (2 * (sink->opener_count + 1) > sink->opener_slot_count &&
        !rehash(&sink->opener_slots, &sink->opener_slot_count, sink->opener_count, sink->openers,
                sizeof(*sink->openers), any_opener_hash)) {
        sink->failed = true;
        return -1;
    }
    const size_t mask = sink->opener_slot_count - 1;
    size_t at = (size_t)opener_hash(&opener) & mask;
    for (; sink->opener_slots[at]; at = (at + 1) & mask) {
        if (same_opener(&sink->openers[sink->opener_slots[at] - 1], &opener)) {
            return (int32_t)(sink->opener_slots[at] - 1);
        }
    }
    markdown_core_context_opener *openers =
        markdown_core_reserve(sink->openers, &sink->opener_capacity, sink->opener_count + 1, sizeof(*openers));
    if (!openers) {
        sink->failed = true;
        return -1;
    }
    sink->openers = openers;
    openers[sink->opener_count] = opener;
    sink->opener_slots[at] = (uint32_t)sink->opener_count + 1;
    return (int32_t)sink->opener_count++;
}

void markdown_core_context_field_add(markdown_core_context_sink *sink, const markdown_core_context_field *field) {
    if (2 * (sink->field_count + 1) > sink->field_slot_count &&
        !rehash(&sink->field_slots, &sink->field_slot_count, sink->field_count, sink->fields, sizeof(*sink->fields),
                any_field_hash)) {
        sink->failed = true;
        return;
    }
    const size_t mask = sink->field_slot_count - 1;
    size_t at = (size_t)field_hash(field) & mask;
    for (; sink->field_slots[at]; at = (at + 1) & mask) {
        if (same_field(&sink->fields[sink->field_slots[at] - 1], field)) {
            return;
        }
    }
    markdown_core_context_field *fields =
        markdown_core_reserve(sink->fields, &sink->field_capacity, sink->field_count + 1, sizeof(*fields));
    if (!fields) {
        sink->failed = true;
        return;
    }
    sink->fields = fields;
    fields[sink->field_count] = *field;
    sink->field_slots[at] = (uint32_t)sink->field_count + 1;
    sink->field_count++;
}

void markdown_core_context_sink_clear(markdown_core_context_sink *sink) {
    markdown_core_free((void *)sink->blocks);
    markdown_core_free(sink->openers);
    markdown_core_free(sink->fields);
    markdown_core_free(sink->opener_slots);
    markdown_core_free(sink->field_slots);
    *sink = (markdown_core_context_sink){.source = sink->source, .length = sink->length};
}
