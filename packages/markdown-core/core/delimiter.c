#include "delimiter.h"

#include <limits.h>
#include <string.h>

#include "extension.h"

#define DELIMITER_INITIAL_CAPACITY 16u

#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
#define DELIMITER_DIAGNOSTIC_ADD(engine, field, value) ((engine)->diagnostics.field += (uint64_t)(value))
#else
#define DELIMITER_DIAGNOSTIC_ADD(engine, field, value) ((void)0)
#endif

static markdown_core_delimiter_record *record_at(
    const markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_id id
) {
    return id && id <= engine->count ? &engine->records[id - 1] : NULL;
}

static int delimiter_rule_is_valid(const markdown_core_delimiter_rule *rule) {
    return rule &&
           (rule->pairing == MARKDOWN_CORE_DELIMITER_PAIR_NEAREST ||
            rule->pairing == MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK) &&
           (rule->reduction == MARKDOWN_CORE_DELIMITER_REDUCE_RANGE ||
            rule->reduction == MARKDOWN_CORE_DELIMITER_REDUCE_ENDPOINTS ||
            rule->reduction == MARKDOWN_CORE_DELIMITER_REDUCE_RUN) &&
           ((rule->close_probe && rule->close_trigger != 0) || (!rule->close_probe && rule->close_trigger == 0));
}

markdown_core_inline_config *markdown_core_inline_config_new(
    markdown_core_mem *mem,
    const int8_t base_special_chars[256],
    const int8_t base_skip_chars[256]
) {
    markdown_core_inline_config *config = (markdown_core_inline_config *)mem->calloc(mem, 1, sizeof(*config));
    if (!config) {
        return NULL;
    }
    config->mem = mem;
    memcpy(config->special_chars, base_special_chars, sizeof(config->special_chars));
    memcpy(config->seam_barrier_chars, base_special_chars, sizeof(config->seam_barrier_chars));
    memcpy(config->skip_chars, base_skip_chars, sizeof(config->skip_chars));
    return config;
}

void markdown_core_inline_attachment_discard(
    markdown_core_inline_config *config,
    markdown_core_inline_attachment *attachment
) {
    if (!attachment) {
        return;
    }
    config->mem->free(config->mem, attachment->rules);
    config->mem->free(config->mem, attachment);
}

void markdown_core_inline_config_free(markdown_core_inline_config *config) {
    markdown_core_inline_attachment *attachment;
    markdown_core_inline_attachment *next;
    size_t i;
    if (!config) {
        return;
    }
    attachment = config->attachments;
    while (attachment) {
        next = attachment->next;
        markdown_core_inline_attachment_discard(config, attachment);
        attachment = next;
    }
    for (i = 0; i < 256; i++) {
        config->mem->free(config->mem, config->dispatch[i].items);
        config->mem->free(config->mem, config->seam_dispatch[i].items);
        config->mem->free(config->mem, config->close_dispatch[i].items);
    }
    config->mem->free(config->mem, config);
}

static void *reserve_pointer_slots(markdown_core_mem *mem, void *items, size_t *capacity, size_t needed) {
    size_t next_capacity;
    void *grown;

    if (needed <= *capacity) {
        return items;
    }
    next_capacity = *capacity ? *capacity : 4;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2) {
            return NULL;
        }
        next_capacity *= 2;
    }
    if (next_capacity > SIZE_MAX / sizeof(void *)) {
        return NULL;
    }
    grown = mem->realloc(mem, items, next_capacity * sizeof(void *));
    if (!grown) {
        return NULL;
    }
    *capacity = next_capacity;
    return grown;
}

markdown_core_inline_attachment *markdown_core_inline_config_find_attachment(
    const markdown_core_inline_config *config,
    const markdown_core_extension *extension
) {
    markdown_core_inline_attachment *attachment;
    if (!config) {
        return NULL;
    }
    for (attachment = config->attachments; attachment; attachment = attachment->next) {
        if (attachment->extension == extension) {
            return attachment;
        }
    }
    return NULL;
}

static int extension_uses_conditional_seam(const markdown_core_extension *extension, unsigned char character) {
    size_t i;
    for (i = 0; i < extension->inline_seam_probe_char_count; i++) {
        if (extension->inline_seam_probe_chars[i] == character) {
            return 1;
        }
    }
    return 0;
}

markdown_core_delimiter_result markdown_core_inline_attachment_prepare(
    markdown_core_inline_config *config,
    markdown_core_extension *extension,
    markdown_core_inline_attachment **prepared
) {
    markdown_core_inline_attachment *attachment;
    size_t dispatch_additions[256] = {0};
    size_t seam_additions[256] = {0};
    size_t close_additions[256] = {0};
    size_t i;
    size_t j;

    if (prepared) {
        *prepared = NULL;
    }
    if (!config || !extension || !prepared || markdown_core_inline_config_find_attachment(config, extension)) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if ((extension->delimiter_rule_count && (!extension->delimiter_rules || !extension->insert_inline_from_delim)) ||
        (extension->special_inline_char_count && !extension->match_inline) ||
        (extension->special_inline_char_count && !extension->special_inline_chars) ||
        (extension->flanking_skip_char_count && !extension->flanking_skip_chars) ||
        (extension->inline_seam_barrier_char_count && !extension->inline_seam_barrier_chars) ||
        (extension->inline_seam_probe_char_count &&
         (!extension->inline_seam_probe_chars || !extension->inline_seam_probe))) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (extension->delimiter_rule_count > UINT16_MAX ||
        config->extension_rule_count >
            SIZE_MAX - MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT - extension->delimiter_rule_count) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    for (i = 0; i < extension->special_inline_char_count; i++) {
        dispatch_additions[extension->special_inline_chars[i]]++;
        for (j = 0; j < i; j++) {
            if (extension->special_inline_chars[i] == extension->special_inline_chars[j]) {
                return MARKDOWN_CORE_DELIMITER_INVALID;
            }
        }
    }
    for (i = 0; i < extension->inline_seam_probe_char_count; i++) {
        int registered = 0;
        unsigned char c = extension->inline_seam_probe_chars[i];
        for (j = 0; j < extension->special_inline_char_count; j++) {
            registered |= extension->special_inline_chars[j] == c;
        }
        for (j = 0; j < i; j++) {
            if (extension->inline_seam_probe_chars[j] == c) {
                return MARKDOWN_CORE_DELIMITER_INVALID;
            }
        }
        if (!registered) {
            return MARKDOWN_CORE_DELIMITER_INVALID;
        }
        seam_additions[c]++;
    }
    for (i = 0; i < extension->delimiter_rule_count; i++) {
        const markdown_core_delimiter_rule *rule = &extension->delimiter_rules[i];
        if (!delimiter_rule_is_valid(rule)) {
            return MARKDOWN_CORE_DELIMITER_INVALID;
        }
        if (rule->close_probe) {
            close_additions[rule->close_trigger]++;
        }
    }

    attachment = (markdown_core_inline_attachment *)config->mem->calloc(config->mem, 1, sizeof(*attachment));
    if (!attachment) {
        return MARKDOWN_CORE_DELIMITER_OOM;
    }
    attachment->extension = extension;
    attachment->rule_count = extension->delimiter_rule_count;

    if (attachment->rule_count) {
        attachment->rules = (markdown_core_delimiter_binding *)
                                config->mem->calloc(config->mem, attachment->rule_count, sizeof(*attachment->rules));
        if (!attachment->rules) {
            markdown_core_inline_attachment_discard(config, attachment);
            return MARKDOWN_CORE_DELIMITER_OOM;
        }
        for (i = 0; i < attachment->rule_count; i++) {
            attachment->rules[i].extension = extension;
            attachment->rules[i].rule = &extension->delimiter_rules[i];
            attachment->rules[i].reduce = extension->insert_inline_from_delim;
            attachment->rules[i].lane = MARKDOWN_CORE_CORE_DELIMITER_RULE_COUNT + config->extension_rule_count + i;
            attachment->rules[i].local_kind = (uint16_t)i;
        }
    }

    for (i = 0; i < 256; i++) {
        if (dispatch_additions[i]) {
            markdown_core_inline_dispatch *bucket = &config->dispatch[i];
            void *grown = reserve_pointer_slots(
                config->mem,
                bucket->items,
                &bucket->capacity,
                bucket->count + dispatch_additions[i]
            );
            if (!grown) {
                markdown_core_inline_attachment_discard(config, attachment);
                return MARKDOWN_CORE_DELIMITER_OOM;
            }
            bucket->items = (markdown_core_inline_attachment **)grown;
        }
        if (seam_additions[i]) {
            markdown_core_inline_dispatch *bucket = &config->seam_dispatch[i];
            void *grown =
                reserve_pointer_slots(config->mem, bucket->items, &bucket->capacity, bucket->count + seam_additions[i]);
            if (!grown) {
                markdown_core_inline_attachment_discard(config, attachment);
                return MARKDOWN_CORE_DELIMITER_OOM;
            }
            bucket->items = (markdown_core_inline_attachment **)grown;
        }
        if (close_additions[i]) {
            markdown_core_inline_close_dispatch *bucket = &config->close_dispatch[i];
            void *grown = reserve_pointer_slots(
                config->mem,
                bucket->items,
                &bucket->capacity,
                bucket->count + close_additions[i]
            );
            if (!grown) {
                markdown_core_inline_attachment_discard(config, attachment);
                return MARKDOWN_CORE_DELIMITER_OOM;
            }
            bucket->items = (markdown_core_delimiter_binding **)grown;
        }
    }
    *prepared = attachment;
    return MARKDOWN_CORE_DELIMITER_OK;
}

void markdown_core_inline_attachment_commit(
    markdown_core_inline_config *config,
    markdown_core_inline_attachment *attachment
) {
    size_t i;
    if (!config->attachments) {
        config->attachments = attachment;
    } else {
        config->attachments_tail->next = attachment;
    }
    config->attachments_tail = attachment;
    config->extension_rule_count += attachment->rule_count;

    for (i = 0; i < attachment->extension->special_inline_char_count; i++) {
        unsigned char c = attachment->extension->special_inline_chars[i];
        markdown_core_inline_dispatch *bucket = &config->dispatch[c];
        bucket->items[bucket->count++] = attachment;
        config->special_chars[c] = 1;
        if (extension_uses_conditional_seam(attachment->extension, c)) {
            markdown_core_inline_dispatch *seam_bucket = &config->seam_dispatch[c];
            seam_bucket->items[seam_bucket->count++] = attachment;
        } else {
            config->seam_barrier_chars[c] = 1;
        }
    }
    for (i = 0; i < attachment->rule_count; i++) {
        markdown_core_delimiter_binding *binding = &attachment->rules[i];
        if (binding->rule->close_probe) {
            unsigned char c = binding->rule->close_trigger;
            markdown_core_inline_close_dispatch *bucket = &config->close_dispatch[c];
            bucket->items[bucket->count++] = binding;
            config->special_chars[c] = 1;
            config->seam_barrier_chars[c] = 1;
        }
    }
    for (i = 0; i < attachment->extension->flanking_skip_char_count; i++) {
        config->skip_chars[attachment->extension->flanking_skip_chars[i]] = 1;
    }
    for (i = 0; i < attachment->extension->inline_seam_barrier_char_count; i++) {
        config->seam_barrier_chars[attachment->extension->inline_seam_barrier_chars[i]] = 1;
    }
}

void markdown_core_delimiter_engine_init(
    markdown_core_delimiter_engine *engine,
    markdown_core_mem *mem,
    size_t lane_count
) {
    memset(engine, 0, sizeof(*engine));
    engine->mem = mem;
    engine->lane_count = lane_count;
}

markdown_core_delimiter_result markdown_core_delimiter_engine_begin(
    markdown_core_delimiter_engine *engine,
    size_t lane_count
) {
    size_t retained_growth;
    if (!engine || !engine->mem || engine->count || engine->tail || !lane_count) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    /* A parser may attach rules between documents. Lanes that become active
     * again must not retain a floor epoch from an earlier rule set,
     * especially across process_epoch wrap. Newly allocated lanes are
     * already zeroed by ensure_lanes. */
    retained_growth = lane_count < engine->lane_capacity ? lane_count : engine->lane_capacity;
    if (engine->lanes && retained_growth > engine->lane_count) {
        memset(engine->lanes + engine->lane_count, 0, (retained_growth - engine->lane_count) * sizeof(*engine->lanes));
    }
    engine->lane_count = lane_count;
    engine->last_claim_order = 0;
    return MARKDOWN_CORE_DELIMITER_OK;
}

void markdown_core_delimiter_engine_free(markdown_core_delimiter_engine *engine) {
    if (engine->records) {
        engine->mem->free(engine->mem, engine->records);
    }
    if (engine->lanes) {
        engine->mem->free(engine->mem, engine->lanes);
    }
    memset(engine, 0, sizeof(*engine));
}

static int ensure_lanes(markdown_core_delimiter_engine *engine) {
    markdown_core_delimiter_lane *lanes;
    if (engine->lane_count <= engine->lane_capacity) {
        return 1;
    }
    if (!engine->lane_count) {
        return 0;
    }
    lanes =
        (markdown_core_delimiter_lane *)engine->mem->calloc(engine->mem, engine->lane_count, sizeof(*engine->lanes));
    if (!lanes) {
        return 0;
    }
    if (engine->lanes) {
        engine->mem->free(engine->mem, engine->lanes);
    }
    engine->lanes = lanes;
    engine->lane_capacity = engine->lane_count;
    return 1;
}

static int ensure_record_capacity(markdown_core_delimiter_engine *engine) {
    size_t capacity;
    size_t bytes;
    void *grown;
    if (engine->count < engine->capacity) {
        return 1;
    }
    capacity = engine->capacity ? engine->capacity * 2 : DELIMITER_INITIAL_CAPACITY;
    if (capacity < engine->capacity || capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(*engine->records)) {
        return 0;
    }
    bytes = capacity * sizeof(*engine->records);
    grown = engine->mem->realloc(engine->mem, engine->records, bytes);
    if (!grown) {
        return 0;
    }
    engine->records = (markdown_core_delimiter_record *)grown;
    engine->capacity = capacity;
    DELIMITER_DIAGNOSTIC_ADD(engine, capacity_growths, 1);
    return 1;
}

markdown_core_delimiter_mark markdown_core_delimiter_engine_mark(const markdown_core_delimiter_engine *engine) {
    markdown_core_delimiter_mark mark;
    mark.count = (uint32_t)engine->count;
    mark.tail = engine->tail;
    return mark;
}

markdown_core_delimiter_result markdown_core_delimiter_engine_push(
    markdown_core_delimiter_engine *engine,
    const markdown_core_delimiter_binding *binding,
    int can_open,
    int can_close,
    markdown_core_node *marker,
    markdown_core_bufsize source_start,
    markdown_core_bufsize source_end,
    uint64_t claim_order
) {
    markdown_core_delimiter_id id;
    markdown_core_delimiter_id open_top;
    markdown_core_delimiter_record *record;
    markdown_core_delimiter_lane *lane;

    if (!engine || !engine->mem || !marker || !binding || !binding->rule || !binding->reduce ||
        binding->lane >= engine->lane_count || (!can_open && !can_close) || source_start < 0 ||
        source_end <= source_start || (binding->rule->close_probe && can_open && can_close) ||
        (binding->rule->close_probe && can_open && claim_order == 0) ||
        (binding->rule->close_probe && can_close && claim_order != 0) ||
        (!binding->rule->close_probe && claim_order != 0) || (claim_order && claim_order <= engine->last_claim_order)) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (engine->tail && record_at(engine, engine->tail)->source_end > source_start) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (!ensure_lanes(engine) || !ensure_record_capacity(engine)) {
        return MARKDOWN_CORE_DELIMITER_OOM;
    }

    id = (markdown_core_delimiter_id)(engine->count + 1);
    record = &engine->records[engine->count++];
    memset(record, 0, sizeof(*record));
    lane = &engine->lanes[binding->lane];

    record->binding = binding;
    record->marker = marker;
    record->source_start = source_start;
    record->source_end = source_end;
    record->original_length = source_end - source_start;
    record->remaining_length = source_end - source_start;
    record->claim_order = claim_order;
    record->can_open = can_open != 0;
    record->can_close = can_close != 0;
    record->active = 1;

    record->previous = engine->tail;
    if (engine->tail) {
        record_at(engine, engine->tail)->next = id;
    }
    engine->tail = id;

    record->previous_rule = lane->tail;
    record->push_previous_rule = lane->tail;
    if (lane->tail) {
        record_at(engine, lane->tail)->next_rule = id;
    }
    lane->tail = id;

    open_top = lane->open_top;
    record->open_top_before = open_top;
    if (binding->rule->close_probe) {
        if (record->can_close && open_top) {
            lane->open_top = record_at(engine, open_top)->previous_open;
        } else if (record->can_open) {
            record->previous_open = open_top;
            lane->open_top = id;
        }
    }
    if (claim_order) {
        engine->last_claim_order = claim_order;
    }
    DELIMITER_DIAGNOSTIC_ADD(engine, pushes, 1);
#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
    if (engine->count > engine->diagnostics.peak_live_records) {
        engine->diagnostics.peak_live_records = engine->count;
    }
#endif
    return MARKDOWN_CORE_DELIMITER_OK;
}

markdown_core_delimiter_id markdown_core_delimiter_engine_last_open(
    const markdown_core_delimiter_engine *engine,
    const markdown_core_delimiter_binding *binding
) {
    if (!engine->lanes || !binding || !binding->rule || !binding->rule->close_probe ||
        binding->lane >= engine->lane_count) {
        return 0;
    }
    return engine->lanes[binding->lane].open_top;
}

uint64_t markdown_core_delimiter_engine_claim_order(
    const markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_id id
) {
    markdown_core_delimiter_record *record = record_at(engine, id);
    return record ? record->claim_order : 0;
}

static void remove_record(markdown_core_delimiter_engine *engine, markdown_core_delimiter_id id) {
    markdown_core_delimiter_record *record = record_at(engine, id);
    markdown_core_delimiter_lane *lane;
    if (!record || !record->active) {
        return;
    }
    lane = &engine->lanes[record->binding->lane];

    if (record->previous) {
        record_at(engine, record->previous)->next = record->next;
    }
    if (record->next) {
        record_at(engine, record->next)->previous = record->previous;
    } else {
        engine->tail = record->previous;
    }

    if (record->previous_rule) {
        record_at(engine, record->previous_rule)->next_rule = record->next_rule;
    }
    if (record->next_rule) {
        record_at(engine, record->next_rule)->previous_rule = record->previous_rule;
    } else {
        lane->tail = record->previous_rule;
    }
    record->active = 0;
    DELIMITER_DIAGNOSTIC_ADD(engine, unlinks, 1);
}

static int pair_is_eligible(
    const markdown_core_delimiter_record *opener,
    const markdown_core_delimiter_record *closer
) {
    if (!opener->can_open) {
        return 0;
    }
    if (closer->binding->rule->pairing != MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK) {
        return 1;
    }
    return !(closer->can_open || opener->can_close) || closer->original_length % 3 == 0 ||
           (opener->original_length + closer->original_length) % 3 != 0;
}

static unsigned int search_class(const markdown_core_delimiter_record *closer) {
    return closer->binding->rule->pairing == MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK
               ? (unsigned int)(closer->original_length % 3)
               : 0;
}

static markdown_core_delimiter_id next_after(markdown_core_delimiter_engine *engine, markdown_core_delimiter_id id) {
    markdown_core_delimiter_record *record = record_at(engine, id);
    return record ? record->next : 0;
}

static void remove_range(
    markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_id opener,
    markdown_core_delimiter_id closer
) {
    markdown_core_delimiter_id id = closer;
    while (id) {
        markdown_core_delimiter_record *record = record_at(engine, id);
        markdown_core_delimiter_id previous = record->previous;
        remove_record(engine, id);
        if (id == opener) {
            break;
        }
        id = previous;
    }
}

static void remove_interior(
    markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_id opener,
    markdown_core_delimiter_id closer
) {
    markdown_core_delimiter_id id = record_at(engine, closer)->previous;
    while (id && id != opener) {
        markdown_core_delimiter_id previous = record_at(engine, id)->previous;
        remove_record(engine, id);
        id = previous;
    }
}

static markdown_core_delimiter_result reduce_pair(
    markdown_core_delimiter_engine *engine,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    markdown_core_delimiter_id opener_id,
    markdown_core_delimiter_id closer_id
) {
    markdown_core_delimiter_record *opener = record_at(engine, opener_id);
    markdown_core_delimiter_record *closer = record_at(engine, closer_id);
    markdown_core_delimiter_match match;
    markdown_core_bufsize use_length = 0;
    markdown_core_delimiter_result result;

    if (closer->binding->rule->reduction == MARKDOWN_CORE_DELIMITER_REDUCE_RUN) {
        use_length = opener->remaining_length >= 2 && closer->remaining_length >= 2 ? 2 : 1;
    }
    memset(&match, 0, sizeof(match));
    match.kind = closer->binding->local_kind;
    match.opener_node = opener->marker;
    match.closer_node = closer->marker;
    match.opener_start = opener->source_start;
    match.opener_end = opener->source_end;
    match.closer_start = closer->source_start;
    match.closer_end = closer->source_end;
    match.opener_length = opener->original_length;
    match.closer_length = closer->original_length;
    match.opener_remaining = opener->remaining_length;
    match.closer_remaining = closer->remaining_length;
    match.use_length = use_length;

    result = closer->binding->reduce(closer->binding->extension, parser, inline_parser, &match);
    if (result < MARKDOWN_CORE_DELIMITER_OK || result > MARKDOWN_CORE_DELIMITER_INVALID) {
        result = MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (result != MARKDOWN_CORE_DELIMITER_OK) {
        return result;
    }
    DELIMITER_DIAGNOSTIC_ADD(engine, reductions, 1);
    DELIMITER_DIAGNOSTIC_ADD(engine, run_bytes_consumed, use_length * 2);

    switch (closer->binding->rule->reduction) {
    case MARKDOWN_CORE_DELIMITER_REDUCE_RANGE:
        remove_range(engine, opener_id, closer_id);
        break;
    case MARKDOWN_CORE_DELIMITER_REDUCE_ENDPOINTS:
        remove_record(engine, opener_id);
        remove_record(engine, closer_id);
        break;
    case MARKDOWN_CORE_DELIMITER_REDUCE_RUN:
        remove_interior(engine, opener_id, closer_id);
        opener->remaining_length -= use_length;
        closer->remaining_length -= use_length;
        if (!opener->remaining_length) {
            remove_record(engine, opener_id);
        }
        if (!closer->remaining_length) {
            remove_record(engine, closer_id);
        }
        break;
    }
    return MARKDOWN_CORE_DELIMITER_OK;
}

static void truncate_to_mark(markdown_core_delimiter_engine *engine, markdown_core_delimiter_mark mark) {
    size_t i;
    if (mark.count > engine->count) {
        return;
    }
    DELIMITER_DIAGNOSTIC_ADD(engine, reclaimed_records, engine->count - mark.count);
    for (i = engine->count; i > mark.count; i--) {
        markdown_core_delimiter_record *record = &engine->records[i - 1];
        markdown_core_delimiter_lane *lane = &engine->lanes[record->binding->lane];
        DELIMITER_DIAGNOSTIC_ADD(engine, truncate_visits, 1);
        lane->tail = record->push_previous_rule;
        lane->open_top = record->open_top_before;
    }
    engine->count = mark.count;
    engine->tail = mark.tail;
    if (engine->tail) {
        record_at(engine, engine->tail)->next = 0;
    }
}

static int mark_is_valid(const markdown_core_delimiter_engine *engine, markdown_core_delimiter_mark mark) {
    return engine && mark.count <= engine->count &&
           (!mark.count ? mark.tail == 0 : mark.tail == mark.count && record_at(engine, mark.tail)->active);
}

markdown_core_delimiter_result markdown_core_delimiter_engine_truncate(
    markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_mark mark
) {
    if (!mark_is_valid(engine, mark)) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    truncate_to_mark(engine, mark);
    return MARKDOWN_CORE_DELIMITER_OK;
}

markdown_core_delimiter_result markdown_core_delimiter_engine_process(
    markdown_core_delimiter_engine *engine,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    markdown_core_delimiter_mark mark
) {
    markdown_core_delimiter_id closer;
    markdown_core_delimiter_result result = MARKDOWN_CORE_DELIMITER_OK;
    uint32_t epoch;

    if (!mark_is_valid(engine, mark)) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (!engine->count || mark.count == engine->count) {
        return MARKDOWN_CORE_DELIMITER_OK;
    }
    DELIMITER_DIAGNOSTIC_ADD(engine, process_calls, 1);
    epoch = ++engine->process_epoch;
    if (!epoch) {
        size_t i;
        for (i = 0; i < engine->lane_count; i++) {
            engine->lanes[i].floor_epoch = 0;
        }
        epoch = ++engine->process_epoch;
    }

    closer = mark.tail ? next_after(engine, mark.tail) : 1;
    while (closer) {
        markdown_core_delimiter_record *closer_record = record_at(engine, closer);
        markdown_core_delimiter_id next = closer_record->next;
        if (closer_record->active && closer_record->can_close) {
            markdown_core_delimiter_lane *lane = &engine->lanes[closer_record->binding->lane];
            unsigned int klass = search_class(closer_record);
            markdown_core_delimiter_id opener;
            markdown_core_delimiter_id floor;

            if (lane->floor_epoch != epoch) {
                lane->floor_epoch = epoch;
                lane->floor[0] = lane->floor[1] = lane->floor[2] = mark.count + 1;
            }
            floor = lane->floor[klass];
            opener = closer_record->previous_rule;
            while (opener >= floor) {
                markdown_core_delimiter_record *opener_record = record_at(engine, opener);
                DELIMITER_DIAGNOSTIC_ADD(engine, opener_candidate_visits, 1);
                if (pair_is_eligible(opener_record, closer_record)) {
                    break;
                }
                opener = opener_record->previous_rule;
            }

            if (opener >= floor) {
                markdown_core_delimiter_reduction reduction = closer_record->binding->rule->reduction;
                result = reduce_pair(engine, parser, inline_parser, opener, closer);
                if (result != MARKDOWN_CORE_DELIMITER_OK) {
                    break;
                }
                if (reduction == MARKDOWN_CORE_DELIMITER_REDUCE_RUN && record_at(engine, closer)->active) {
                    next = closer;
                } else {
                    next = next_after(engine, closer);
                }
            } else {
                lane->floor[klass] = closer;
                if (!closer_record->can_open) {
                    remove_record(engine, closer);
                }
            }
        }
        closer = next;
    }
    truncate_to_mark(engine, mark);
    return result;
}

#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
const markdown_core_delimiter_diagnostics *markdown_core_delimiter_engine_diagnostics(
    const markdown_core_delimiter_engine *engine
) {
    return &engine->diagnostics;
}

int markdown_core_delimiter_engine_validate(const markdown_core_delimiter_engine *engine) {
    markdown_core_delimiter_id global_previous = 0;
    markdown_core_bufsize previous_source_end = 0;
    uint64_t previous_claim_order = 0;
    size_t i;

    if (!engine || engine->count > engine->capacity || engine->count > UINT32_MAX ||
        (engine->count && (!engine->records || !engine->lanes))) {
        return 0;
    }

    for (i = 0; i < engine->count; i++) {
        markdown_core_delimiter_id id = (markdown_core_delimiter_id)(i + 1);
        const markdown_core_delimiter_record *record = &engine->records[i];
        const markdown_core_delimiter_record *related;

        if (!record->active) {
            continue;
        }
        if (!record->binding || !record->binding->rule || !record->binding->reduce ||
            record->binding->lane >= engine->lane_count || record->source_start < 0 ||
            record->source_end <= record->source_start ||
            record->original_length != record->source_end - record->source_start || record->remaining_length <= 0 ||
            record->remaining_length > record->original_length || record->previous != global_previous ||
            (!record->can_open && !record->can_close) ||
            (record->binding->rule->close_probe && record->can_open && record->can_close) ||
            (record->binding->rule->close_probe && record->can_open && !record->claim_order) ||
            (record->binding->rule->close_probe && record->can_close && record->claim_order) ||
            (!record->binding->rule->close_probe && record->claim_order) ||
            (global_previous && record->source_start < previous_source_end) ||
            (record->claim_order && record->claim_order <= previous_claim_order)) {
            return 0;
        }
        if (record->previous) {
            related = record_at(engine, record->previous);
            if (!related || !related->active || record->previous >= id || related->next != id) {
                return 0;
            }
        }
        if (record->next) {
            related = record_at(engine, record->next);
            if (!related || !related->active || record->next <= id || related->previous != id) {
                return 0;
            }
        } else if (engine->tail != id) {
            return 0;
        }
        if (record->previous_rule) {
            related = record_at(engine, record->previous_rule);
            if (!related || !related->active || record->previous_rule >= id ||
                related->binding->lane != record->binding->lane || related->next_rule != id) {
                return 0;
            }
        }
        if (record->next_rule) {
            related = record_at(engine, record->next_rule);
            if (!related || !related->active || record->next_rule <= id ||
                related->binding->lane != record->binding->lane || related->previous_rule != id) {
                return 0;
            }
        } else if (engine->lanes[record->binding->lane].tail != id) {
            return 0;
        }
        global_previous = id;
        previous_source_end = record->source_end;
        if (record->claim_order) {
            previous_claim_order = record->claim_order;
        }
    }

    if (global_previous != engine->tail || previous_claim_order > engine->last_claim_order ||
        (engine->tail && record_at(engine, engine->tail)->next)) {
        return 0;
    }

    if (!engine->lanes) {
        return engine->count == 0 && engine->tail == 0;
    }
    for (i = 0; i < engine->lane_count; i++) {
        markdown_core_delimiter_id tail = engine->lanes[i].tail;
        markdown_core_delimiter_id opener = engine->lanes[i].open_top;
        const markdown_core_delimiter_record *record;
        if (tail) {
            record = record_at(engine, tail);
            if (!record || !record->active || record->binding->lane != i || record->next_rule) {
                return 0;
            }
        }
        if (opener) {
            record = record_at(engine, opener);
            if (!record || !record->active || !record->can_open || !record->binding->rule->close_probe ||
                record->binding->lane != i) {
                return 0;
            }
        }
    }
    return 1;
}
#endif
