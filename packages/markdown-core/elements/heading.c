#include "alloc.h"
#include "heading_scanners.h"
#include "citation.h"
#include "heading.h"
#include "link.h"
#include "directive.h"
#include "inline_internal.h"
#include "block_internal.h"
#include "attributes.h"
#include "topic.h"
#include "parts.h"
#include "ast_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { HEADING_CITATION };
/* A heading whose inline content is parsed (heading_state.h). */
enum { MARKDOWN_CORE_NODE__HEADING_PARSED = MARKDOWN_CORE_NODE__ELEMENT_FIRST };
static const markdown_core_element *const HEADING_PEERS[] = {[HEADING_CITATION] = &MARKDOWN_CORE_ELEMENT_CITATION,
                                                             NULL};
typedef enum { ANCHOR_CONTENT, ANCHOR_CITATIONS, ANCHOR_KEY } anchor_projection_kind;
typedef struct {
    markdown_core_node *node;
    anchor_projection_kind kind;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;
/* The headings' zipper (heading_state.h): an entry's one place is where
 * its heading starts, and the front keeps no index of its own. */
static inline markdown_core_heading_parse heading_flip(markdown_core_heading_parse entry, size_t end) {
    entry.start = (uint32_t)(end - entry.start);
    return entry;
}
static inline size_t heading_place(const markdown_core_heading_parse *entry) { return entry->start; }
static inline void heading_leave(markdown_core_heading_state *state, size_t first) {
    (void)state;
    (void)first;
}
MARKDOWN_CORE_ZIPPER_OPS(heading_zipper, markdown_core_heading_zipper, markdown_core_heading_parse, heading_flip,
                         heading_place, heading_leave, markdown_core_heading_state)

/* The front, which holds `count` of the reading's headings: a reading put
 * back to a mark left the entries past it (heading_state.h). */
static inline void heading_sync(markdown_core_heading_state *state) {
    if (state->headings.front.count > state->count) {
        markdown_core_array_truncate(&state->headings.front, state->count);
    }
}

/* How many headings the parse holds, the old tail's it took back among
 * them, and heading `at` of them in source order, to change in place. */
static inline size_t heading_total(const markdown_core_heading_state *state) {
    return state->count + (state->spliced ? state->headings.back_count : 0);
}
static inline markdown_core_heading_parse *heading_at(markdown_core_heading_state *state, size_t at) {
    markdown_core_heading_zipper *headings = &state->headings;
    return at < headings->front.count ? markdown_core_array_write(&headings->front, at)
                                      : &headings->back[headings->back_count - 1 - (at - headings->front.count)];
}

/* A heading closes into the element's collection, which declares its label
 * and assigns its anchor as the parse ends. */
static bool close_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, markdown_core_node *parent) {
    (void)parent;
    markdown_core_heading_state *state = self->state;
    heading_sync(state);
    if (!markdown_core_array_push(&state->headings.front,
                                  ((markdown_core_heading_parse){.node = node, .start = node->place.start}))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return true;
    }
    state->count++;
    return true;
}

static uint64_t heading_start_key(const void *entry) { return ((const markdown_core_heading_parse *)entry)->start; }

/* The inline content of every heading that settled since the last
 * completion: first, in source order, each one's inlines up to its
 * declaration dependency and the implicit reference it declares, then the
 * rest of their inlines. The headings before the collection's `parsed` come
 * before every later one in source order, so only the headings after them
 * are put in order: those that closed since, and those the last completion
 * left because they had not settled. */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    heading_sync(state);
    const size_t first = state->parsed, count = state->count;
    if (first < count &&
        !markdown_core_order_source_entries(&parser->source_order, heading_at(state, first), count - first,
                                            sizeof(markdown_core_heading_parse), heading_start_key)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    /* Anchors are numbered in source order, whatever order mapped inputs were
     * scheduled and headings closed in. */
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        if (!(heading->node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) &&
            markdown_core_parser_settled(parser, heading->node)) {
            *heading = (markdown_core_heading_parse){.node = heading->node, .start = heading->start};
            parser->completing = heading->node;
            markdown_core_prepare_heading(self, parser, heading);
        }
    }
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        markdown_core_node *node = heading->node;
        if ((node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) || !markdown_core_parser_settled(parser, node)) {
            continue;
        }
        parser->completing = node;
        markdown_core_finish_heading(parser, heading);
        node->flags |= MARKDOWN_CORE_NODE__HEADING_PARSED;
        heading->authored = node->attributes.anchor.len != 0;
    }
    /* A failed parse lets go the inline states it held for the headings. */
    for (size_t i = first; parser->error && i < count; i++) {
        markdown_core_dispose_heading(heading_at(state, i));
    }
    while (state->parsed < count &&
           (state->headings.front.values[state->parsed].node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED)) {
        state->parsed++;
    }
}

/* The parsed heading that starts at `start`, found by halves among those in
 * source order. */
static markdown_core_heading_parse *parsed_heading(markdown_core_heading_state *state, uint32_t start) {
    heading_sync(state);
    const markdown_core_heading_parse *values = state->headings.front.values;
    size_t low = 0, high = state->parsed;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (values[middle].start < start) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    assert(low < state->parsed && values[low].start == start);
    return heading_at(state, low);
}

/* A heading parsed again (element.h, `reparse_inlines`), a candidate of a
 * heading a streaming commit settled: the attributes its last line ends with
 * are its inline content's and go with it, and the label its text declares,
 * whatever a lookup answers, stays declared. The anchor the headings
 * computed for it stays while its attributes write none; the heading's entry
 * learns whether they do, and that the base of its anchor is to be projected
 * again. */
static void reparse_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *node) {
    markdown_core_chunk computed = {0};
    if (node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) {
        computed = node->attributes.anchor;
        node->attributes.anchor = (markdown_core_chunk){0};
        node->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__ANCHOR_COMPUTED;
    }
    markdown_core_attributes_free(&node->attributes);
    markdown_core_heading_parse heading = {.node = node};
    markdown_core_prepare_heading(self, parser, &heading);
    markdown_core_finish_heading(parser, &heading);
    const bool authored = node->attributes.anchor.len != 0;
    if (authored) {
        markdown_core_chunk_free(&computed);
    } else if (computed.len) {
        markdown_core_chunk_free(&node->attributes.anchor);
        node->attributes.anchor = computed;
        node->flags |= MARKDOWN_CORE_NODE__ANCHOR_COMPUTED;
    }
    markdown_core_heading_parse *entry = parsed_heading(self->state, node->place.start);
    entry->authored = authored;
    if (entry->holds_name) {
        markdown_core_free((void *)entry->name);
        entry->holds_name = false;
    }
    entry->name = NULL;
}

/* The entries `values` holds, `count` of them, let go, with the names
 * they hold. */
static void headings_let_go(markdown_core_heading_parse *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        markdown_core_dispose_heading(&values[i]);
        if (values[i].holds_name) {
            markdown_core_free((void *)values[i].name);
        }
    }
}

/* THE NAMES AN EDIT MOVES. A heading of base p takes p or p-k (anchors.md),
 * so two names bear on each other only when one is the other with one
 * suffix `-k` more: p-k is q-j for no q other than p. A stem's parent is
 * its name less that suffix, and a stem with headings, a present one, and
 * its parent, when present too, are one group: the names of a group are
 * the names a pass over its headings alone gives them, in order, past the
 * anchors that stand, whatever the other groups hold. An edit names again
 * only the groups of the stems it touched: the bases of the headings its
 * reading let go and of those it read, and the stems an anchor it made or
 * let go names, with their parents (heading_rename). Each stem keeps where
 * its headings start in a zipper of its own about the headings' focus, so
 * that a group's headings come in order from its stems' starts, each found
 * by halves among the headings. The first reopen makes the stems; a parse
 * no edit reopened names its headings as THE COMMITTED NAMES say. */
static inline uint32_t stem_flip(uint32_t start, size_t end) { return (uint32_t)(end - start); }
static inline size_t stem_place(const uint32_t *start) { return *start; }
static inline void stem_leave(markdown_core_heading_stem *stem, size_t first) {
    (void)stem;
    (void)first;
}
MARKDOWN_CORE_ZIPPER_OPS(stem_zipper, markdown_core_heading_starts, uint32_t, stem_flip, stem_place, stem_leave,
                         markdown_core_heading_stem)

static inline markdown_core_heading_stem *stem_of(markdown_core_heading_names *names, uint32_t id) {
    return &names->stems[id - 1];
}

/* How many bytes of `key`, `size` of them, its parent is: the name less a
 * last suffix `-k`, k a number of no leading zero; zero when it has none. */
static bufsize_t stem_parent(const unsigned char *key, bufsize_t size) {
    bufsize_t at = size;
    while (at > 0 && key[at - 1] >= '0' && key[at - 1] <= '9') {
        at--;
    }
    if (at == size || key[at] == '0' || at < 2 || key[at - 1] != '-') {
        return 0;
    }
    return at - 1;
}

/* The stem of the name, its place plus one, or zero when none is kept. */
static inline uint32_t stem_find(const markdown_core_heading_names *names, const unsigned char *key, bufsize_t size) {
    const markdown_core_key_index_slot *slot = markdown_core_key_index_find(&names->index, key, size);
    return slot ? (uint32_t)slot->value : 0;
}

/* The stem of a heading that names itself, from the base its name holds. */
static inline uint32_t stem_heading(const markdown_core_heading_names *names,
                                    const markdown_core_heading_parse *heading) {
    return heading->authored || !heading->name ? 0 : stem_find(names, heading->name, heading->base_size);
}

/* The stem of the name, made when none is kept: zero when storage could
 * not be had. */
static uint32_t stem_make(markdown_core_heading_names *names, const unsigned char *key, bufsize_t size) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&names->index, key, size);
    if (!slot) {
        return 0;
    }
    if (slot->key) {
        return (uint32_t)slot->value;
    }
    unsigned char *bytes = markdown_core_alloc((size_t)size + 1, 1);
    if (!bytes) {
        return 0;
    }
    uint32_t id = names->spare;
    if (id) {
        names->spare = stem_of(names, id)->next;
    } else {
        markdown_core_heading_stem *stems =
            markdown_core_reserve(names->stems, &names->stem_room, names->stem_count + 1, sizeof(*stems));
        if (!stems) {
            markdown_core_free(bytes);
            return 0;
        }
        names->stems = stems;
        id = (uint32_t)++names->stem_count;
    }
    memcpy(bytes, key, (size_t)size);
    markdown_core_heading_stem *stem = stem_of(names, id);
    *stem = (markdown_core_heading_stem){.key = bytes, .size = size};
    markdown_core_key_index_commit(&names->index, slot, bytes);
    slot->value = id;
    return id;
}

static inline bool stem_present(const markdown_core_heading_stem *stem) {
    return stem_zipper_count(&stem->starts) != 0;
}

/* The stem names again once the edit's reading is done. False when storage
 * could not be had. */
static bool stem_touch(markdown_core_heading_names *names, uint32_t id) {
    if (names->touched_count == names->touched_room) {
        uint32_t *touched =
            markdown_core_reserve(names->touched, &names->touched_room, names->touched_count + 1, sizeof(*touched));
        if (!touched) {
            return false;
        }
        names->touched = touched;
    }
    names->touched[names->touched_count++] = id;
    return true;
}

/* The stem, present now, joins its parent's children, the parent made when
 * none is kept. False when storage could not be had. */
static bool stem_link(markdown_core_heading_names *names, uint32_t id) {
    const markdown_core_heading_stem *stem = stem_of(names, id);
    const bufsize_t size = stem_parent(stem->key, stem->size);
    if (!size) {
        return true;
    }
    const uint32_t parent = stem_make(names, stem_of(names, id)->key, size);
    if (!parent || !stem_touch(names, parent)) {
        return false;
    }
    markdown_core_heading_stem *own = stem_of(names, id), *up = stem_of(names, parent);
    own->prior = 0;
    own->next = up->first;
    if (up->first) {
        stem_of(names, up->first)->prior = id;
    }
    up->first = id;
    return true;
}

/* The stem, no longer present, leaves its parent's children. */
static void stem_unlink(markdown_core_heading_names *names, uint32_t id) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    const bufsize_t size = stem_parent(stem->key, stem->size);
    if (!size) {
        return;
    }
    const uint32_t parent = stem_find(names, stem->key, size);
    assert(parent);
    if (stem->prior) {
        stem_of(names, stem->prior)->next = stem->next;
    } else {
        stem_of(names, parent)->first = stem->next;
    }
    if (stem->next) {
        stem_of(names, stem->next)->prior = stem->prior;
    }
    stem->next = stem->prior = 0;
}

/* The stem, with no heading and no child, goes, its place then free. */
static void stem_release(markdown_core_heading_names *names, uint32_t id) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    if (!stem->key || stem_present(stem) || stem->first) {
        return;
    }
    markdown_core_key_index_remove(&names->index, markdown_core_key_index_find(&names->index, stem->key, stem->size));
    markdown_core_free(stem->key);
    markdown_core_array_free(&stem->starts.front);
    stem_zipper_release_back(&stem->starts);
    *stem = (markdown_core_heading_stem){.next = names->spare};
    names->spare = id;
}

/* A heading of the stem starts at `start` of the text, after each of the
 * stem's headings the front holds. False when storage could not be had. */
static bool stem_add(markdown_core_heading_names *names, uint32_t id, uint32_t start) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    const bool present = stem_present(stem);
    if (!markdown_core_array_push(&stem->starts.front, start)) {
        return false;
    }
    return (present || stem_link(names, id)) && stem_touch(names, id);
}

/* The stem's headings the back holds that start before `place` of the
 * text, its end `end`, go. False when storage could not be had. */
static bool stem_cut(markdown_core_heading_names *names, uint32_t id, size_t place, size_t end) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    stem->starts.end = end;
    (void)stem_zipper_cut(&stem->starts, place);
    if (!stem_present(stem)) {
        stem_unlink(names, id);
    }
    return stem_touch(names, id);
}

/* The headings' names, kept from now on as stems (THE NAMES AN EDIT MOVES):
 * a flushed parse named every heading that does not author its anchor, and
 * the front holds them all. False when storage could not be had. */
static bool stems_make(markdown_core_heading_state *state) {
    markdown_core_heading_names *names = markdown_core_alloc(1, sizeof(*names));
    if (!names || !markdown_core_key_index_init(&names->index, state->count)) {
        markdown_core_free(names);
        return false;
    }
    state->names = names;
    const markdown_core_heading_parse *values = state->headings.front.values;
    for (size_t i = 0; i < state->count; i++) {
        const markdown_core_heading_parse *heading = &values[i];
        if (heading->authored || !heading->name) {
            continue;
        }
        const uint32_t id = stem_make(names, heading->name, heading->base_size);
        if (!id || !stem_add(names, id, heading->start)) {
            return false;
        }
    }
    /* What the stems touched names nothing again: every name holds. */
    names->touched_count = 0;
    markdown_core_key_index_free(&state->taken);
    return true;
}

static void stems_free(markdown_core_heading_names *names) {
    if (!names) {
        return;
    }
    for (size_t i = 0; i < names->stem_count; i++) {
        markdown_core_free(names->stems[i].key);
        markdown_core_array_free(&names->stems[i].starts.front);
        stem_zipper_release_back(&names->stems[i].starts);
    }
    markdown_core_free(names->stems);
    markdown_core_key_index_free(&names->index);
    markdown_core_free(names->touched);
    markdown_core_free(names);
}

/* Each heading the move took to the other part takes its stem's focus to
 * `place` as well: those of the front's past `front` before the move, or
 * of the back's past `back`. */
static void stems_follow(markdown_core_heading_state *state, size_t place, size_t front, size_t back) {
    markdown_core_heading_names *names = state->names;
    markdown_core_heading_zipper *headings = &state->headings;
    for (size_t i = back; i < headings->back_count; i++) {
        const uint32_t id = stem_heading(names, &headings->back[i]);
        if (id) {
            stem_of(names, id)->starts.end = headings->end;
            (void)stem_zipper_move(&stem_of(names, id)->starts, place, headings->end, stem_of(names, id));
        }
    }
    for (size_t i = front; i < headings->front.count; i++) {
        const uint32_t id = stem_heading(names, &headings->front.values[i]);
        if (id) {
            stem_of(names, id)->starts.end = headings->end;
            (void)stem_zipper_move(&stem_of(names, id)->starts, place, headings->end, stem_of(names, id));
        }
    }
}

/* Room for each stem the move takes a heading of to the other part to
 * follow it (stems_follow), which then cannot fail: those of the front's
 * from `first` on, or of the back's that start before `place`. */
static bool stems_ready(markdown_core_heading_state *state, size_t place, size_t size) {
    markdown_core_heading_names *names = state->names;
    markdown_core_heading_zipper *headings = &state->headings;
    const size_t first = heading_zipper_front_from(headings, place);
    const size_t taken = headings->back_count ? heading_zipper_back_from(headings, place) : 0;
    for (size_t i = first; i < headings->front.count; i++) {
        const uint32_t id = stem_heading(names, &headings->front.values[i]);
        if (id) {
            stem_of(names, id)->starts.end = size;
            if (!stem_zipper_ready(&stem_of(names, id)->starts, place, size)) {
                return false;
            }
        }
    }
    for (size_t k = 0; k < taken; k++) {
        const uint32_t id = stem_heading(names, &headings->back[headings->back_count - 1 - k]);
        if (id) {
            stem_of(names, id)->starts.end = size;
            if (!stem_zipper_ready(&stem_of(names, id)->starts, place, size)) {
                return false;
            }
        }
    }
    return true;
}

/* A flushed parse names every heading and parses each, so its headings
 * are in source order: the focus moves to `start`, those from the first
 * that starts there or past it going to the back, each start measured back
 * from the text's end (THE BACK, blocks.c), and the back's before it coming
 * to the front. */
bool markdown_core_headings_reopen(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   uint32_t start) {
    markdown_core_heading_state *state = self->state;
    heading_sync(state);
    assert(state->parsed == state->count && (state->spliced || !state->headings.back_count));
    const size_t size = markdown_core_parser_back_end(parser);
    if ((!state->names && !stems_make(state)) || !stems_ready(state, start, size)) {
        return false;
    }
    const size_t front = state->headings.front.count, back = state->headings.back_count;
    if (!heading_zipper_move(&state->headings, start, size, state)) {
        return false;
    }
    stems_follow(state, start, front, back);
    const size_t low = state->headings.front.count;
    state->names->rest = start;
    state->names->fresh = low;
    state->count = state->parsed = low;
    state->spliced = false;
    size_t renamed = 0;
    for (size_t i = 0; i < state->renamed_count; i++) {
        if (state->renamed[i] < low) {
            state->renamed[renamed++] = state->renamed[i];
        }
    }
    state->renamed_count = renamed;
    if (state->named > low) {
        state->named = low;
    }
    return true;
}

/* The back's headings, from the focus, that start before `point` of the
 * new text go, the reading's own read again: the rest are the old tail
 * the reading takes back, where they stood from the end. */
bool markdown_core_headings_splice(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   uint32_t point) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_zipper *headings = &state->headings;
    heading_sync(state);
    /* The back's are cut in the old text's terms, the end the reopen
     * measured them from: those before the edit moved with it. */
    const size_t end = markdown_core_parser_back_end(parser);
    const size_t old = (size_t)((int64_t)point - ((int64_t)end - (int64_t)headings->end));
    const size_t gone = heading_zipper_back_from(headings, old);
    for (size_t k = 0; k < gone; k++) {
        const uint32_t id = stem_heading(state->names, &headings->back[headings->back_count - 1 - k]);
        if (id && !stem_cut(state->names, id, old, headings->end)) {
            return false;
        }
    }
    headings_let_go(headings->back + headings->back_count - gone, gone);
    headings->back_count -= gone;
    headings->end = end;
    state->spliced = true;
    return true;
}

/* The old tail a reading did not take back goes: it read every line. */
static bool headings_drop_back(markdown_core_heading_state *state) {
    markdown_core_heading_zipper *headings = &state->headings;
    if (state->spliced || !headings->back_count) {
        return true;
    }
    for (size_t i = 0; i < headings->back_count; i++) {
        const uint32_t id = stem_heading(state->names, &headings->back[i]);
        if (id && !stem_cut(state->names, id, SIZE_MAX, headings->end)) {
            return false;
        }
    }
    headings_let_go(headings->back, headings->back_count);
    headings->back_count = 0;
    return true;
}

/* Release the headings and the names they took, with the committed names
 * their entries hold. The anchors are assigned by then, or the parse failed;
 * either way nothing reads them again. */
void markdown_core_headings_dispose(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    heading_sync(state);
    headings_let_go(state->headings.front.values, state->count);
    headings_let_go(state->headings.back, state->headings.back_count);
    markdown_core_array_free(&state->headings.front);
    heading_zipper_release_back(&state->headings);
    state->count = state->parsed = 0;
    state->spliced = false;
    stems_free(state->names);
    state->names = NULL;
    markdown_core_key_index_free(&state->taken);
    markdown_core_free(state->renamed);
    state->renamed = NULL;
    state->renamed_count = state->renamed_capacity = 0;
    state->records_at = state->records_read = state->named = 0;
}

static void project_anchor_literal(markdown_core_heading_state *state, markdown_core_strbuf *base,
                                   const unsigned char *text, bufsize_t length) {
    state->anchor_work += (size_t)length;
    markdown_core_utf8proc_anchor(base, text, length);
}

static bool push_anchor_projection(markdown_core_parser *parser, anchor_projection_stack *stack,
                                   markdown_core_node *node, anchor_projection_kind kind) {
    if (!node) {
        return true;
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*stack->values)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        void *values = markdown_core_realloc(stack->values, capacity * sizeof(*stack->values));
        if (!values) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
        stack->values = values;
        stack->capacity = capacity;
    }
    stack->values[stack->count++] = (anchor_projection){node, kind};
    return true;
}

/* `node`'s children, pushed last first so that they are projected in order;
 * none for no node. */
static bool push_anchor_children(markdown_core_parser *parser, anchor_projection_stack *stack,
                                 const markdown_core_node *node, anchor_projection_kind kind) {
    for (uint32_t i = node ? node->child_count : 0; i > 0; i--) {
        if (!push_anchor_projection(parser, stack, node->children[i - 1], kind)) {
            return false;
        }
    }
    return true;
}

/* The heading's anchor base, projected from its inlines and appended to
 * `base`. The stack's storage is the caller's and serves every heading of the
 * pass, as `base` does; each heading starts it empty. */
static void heading_anchor_base(markdown_core_parser *parser, markdown_core_heading_state *state,
                                markdown_core_node *heading, markdown_core_strbuf *base,
                                anchor_projection_stack *stack) {
    const bufsize_t start = base->size;
    stack->count = 0;
    push_anchor_children(parser, stack, heading, ANCHOR_CONTENT);
    while (stack->count && !parser->error && !base->oom) {
        anchor_projection projection = stack->values[--stack->count];
        markdown_core_node *node = projection.node;
        state->anchor_work++;
        if (projection.kind == ANCHOR_KEY) {
            project_anchor_literal(state, base, (const unsigned char *)"@", 1);
            project_anchor_literal(state, base, node->as.citation->value.data, node->as.citation->value.len);
            continue;
        }
        if (projection.kind == ANCHOR_CITATIONS) {
            markdown_core_citation_item *item = node->as.citation;
            if (item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
                push_anchor_children(parser, stack, item->suffix, ANCHOR_CONTENT);
                push_anchor_projection(parser, stack, node, ANCHOR_KEY);
                push_anchor_children(parser, stack, item->prefix, ANCHOR_CONTENT);
            } else if (item->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
                push_anchor_projection(parser, stack, node, ANCHOR_KEY);
            }
            continue;
        }
        switch (node->kind) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_CODE:
            project_anchor_literal(state, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_TOPIC:
            project_anchor_literal(state, base, (const unsigned char *)"#", 1);
            project_anchor_literal(state, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_FORMULA: {
            const markdown_core_chunk literal = markdown_core_elements_formula_literal(node);
            project_anchor_literal(state, base, literal.data, literal.len);
            break;
        }
        case MARKDOWN_CORE_NODE_SOFT_BREAK:
        case MARKDOWN_CORE_NODE_LINE_BREAK:
            markdown_core_strbuf_putc(base, '-');
            break;
        case MARKDOWN_CORE_NODE_CROSS_LINK:
        case MARKDOWN_CORE_NODE_CROSS_EMBEDDED: {
            markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
            if (cross->label.has_value) {
                project_anchor_literal(state, base, cross->label.value.data, cross->label.value.len);
            } else {
                project_anchor_literal(state, base, cross->path.data, cross->path.len);
                if (cross->anchor.has_value) {
                    project_anchor_literal(state, base, cross->anchor.value.data, cross->anchor.value.len);
                }
            }
            break;
        }
        case MARKDOWN_CORE_NODE_CITE:
            push_anchor_children(parser, stack, node, ANCHOR_CITATIONS);
            break;
        case MARKDOWN_CORE_NODE_EMPHASIS:
        case MARKDOWN_CORE_NODE_STRONG:
        case MARKDOWN_CORE_NODE_STRIKETHROUGH:
        case MARKDOWN_CORE_NODE_MARK:
        case MARKDOWN_CORE_NODE_INSERTION:
        case MARKDOWN_CORE_NODE_SPAN:
        case MARKDOWN_CORE_NODE_SUPERSCRIPT:
        case MARKDOWN_CORE_NODE_SUBSCRIPT:
        case MARKDOWN_CORE_NODE_LINK:
        case MARKDOWN_CORE_NODE_EMBEDDED:
        case MARKDOWN_CORE_NODE_DIRECTIVE_LABEL:
            push_anchor_children(parser, stack, node, ANCHOR_CONTENT);
            break;
        case MARKDOWN_CORE_NODE_DIRECTIVE: {
            push_anchor_children(parser, stack, markdown_core_directive_label(node), ANCHOR_CONTENT);
            break;
        }
        default:
            /* HTML, comments and other opaque values contribute no text. */
            break;
        }
    }
    if (base->size == start) {
        markdown_core_strbuf_puts(base, "section");
    }
    if (base->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

static void append_anchor_suffix(markdown_core_strbuf *base, size_t ordinal) {
    char suffix[3 * sizeof(size_t) + 1];
    char *end = suffix + sizeof(suffix), *start = end;
    do {
        *--start = (char)('0' + ordinal % 10);
        ordinal /= 10;
    } while (ordinal);
    *--start = '-';
    markdown_core_strbuf_put(base, (const unsigned char *)start, (bufsize_t)(end - start));
}

/* THE COMMITTED NAMES. A heading's anchor is the first name of its sequence
 * -- its base, then the base with -1, -2 and so on -- that no explicit anchor
 * reserves and no heading before it was given (anchors.md). The names taken
 * are one index. A name's entry holds who takes it, in `extra`: zero for an
 * explicit anchor, a heading's place in the collection plus one otherwise;
 * and in `value` the suffix a heading projecting to the name tries next. Its
 * key is the taker's bytes: the label of the roster record that reserves
 * it, or the bytes of the heading's anchor.
 *
 * A heading takes the first name of its sequence, from where it starts, that
 * is vacant or that a later heading holds, which then goes on from the
 * suffix of that name (it is displaced). Taking a name moves a heading on
 * along its sequence and frees no name, so the names everything before a
 * heading takes only grow, and the result is the one a pass over the
 * reservations and then the headings in order gives. A heading after every
 * other starts from its base and then the base's next suffix, which it
 * advances, and displaces no one; a displaced heading starts past its own
 * name, and does not read the suffix, below which later headings can hold
 * names it may take.
 *
 * A streaming parse keeps the index across snapshots, with where its reading
 * of the roster's ANCHOR records stands and how many headings it named.
 * Each snapshot first reads the records committed since the last and names
 * the headings commits parsed since, then names its tail in an overlay (A
 * SNAPSHOT'S NAMES). The finalize, and a parse that takes no snapshot, start
 * the index empty and read every record, and then every heading, which
 * displaces no one. Until some heading asks for a name, no record is read. */
typedef struct {
    markdown_core_parser *parser;
    markdown_core_heading_state *state;
    /* The index the pass writes, the committed names or a snapshot's
     * overlay; and the committed names beneath an overlay, or NULL. */
    markdown_core_key_index *index;
    const markdown_core_key_index *beneath;
    /* Whether the pass starts empty, which reads only the declarations that
     * stand: the names they reserve are what reading every record would
     * leave, before any heading takes one. */
    bool fresh;
    /* The candidate name, and the projection's stack, which every heading
     * of the pass uses in turn. */
    markdown_core_strbuf name;
    anchor_projection_stack stack;
} name_pass;

/* The overlay's entry for `name`, which first takes the committed entry of a
 * name it has none for. */
static markdown_core_key_index_slot *overlay_entry(name_pass *pass, const unsigned char *name, bufsize_t length) {
    markdown_core_key_index_slot *entry = markdown_core_key_index_entry(pass->index, name, length);
    if (entry && !entry->key) {
        const markdown_core_key_index_slot *committed = markdown_core_key_index_find(pass->beneath, name, length);
        if (committed) {
            markdown_core_key_index_commit(pass->index, entry, committed->key);
            entry->value = committed->value;
            entry->extra = committed->extra;
        }
    }
    return entry;
}

/* The entry for `name` among the names taken, occupied or vacant, in the
 * index the pass writes. NULL when the index could not grow. */
static inline markdown_core_key_index_slot *name_entry(name_pass *pass, const unsigned char *name, bufsize_t length) {
    pass->state->anchor_work += (size_t)length + 1;
    return pass->beneath ? overlay_entry(pass, name, length) : markdown_core_key_index_entry(pass->index, name, length);
}

/* `made`, bytes the call takes, given to `node` as its computed anchor. The
 * anchor the node computed before goes to `*kept` when `kept` is not NULL,
 * and is freed otherwise: the document the last publish made is read no
 * longer. The node's bytes. */
static inline const unsigned char *show_anchor(markdown_core_node *node, markdown_core_chunk made,
                                               unsigned char **kept) {
    markdown_core_chunk *anchor = &node->attributes.anchor;
    if (node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) {
        if (kept) {
            *kept = anchor->data;
        } else {
            markdown_core_chunk_free(anchor);
        }
    }
    *anchor = made;
    node->flags |= MARKDOWN_CORE_NODE__ANCHOR_COMPUTED;
    return anchor->data;
}

/* A COMMITTED HEADING UNDER ANOTHER NAME. A snapshot that displaces a
 * committed heading gives its node the name the overlay gives it, and the
 * bytes of its committed name, the committed index's key, go to its entry
 * (`holds_name`), which the published document that reads them no longer
 * reads once the next publish is done. The snapshot lists the heading; the
 * next one gives the bytes back to the node unless it displaces the heading
 * too, and committed names that move the heading on free them. */
static bool rename_committed(markdown_core_parser *parser, markdown_core_heading_state *state, size_t at) {
    markdown_core_heading_parse *heading = heading_at(state, at);
    if (heading->renamed) {
        return true;
    }
    uint32_t *renamed =
        markdown_core_reserve(state->renamed, &state->renamed_capacity, state->renamed_count + 1, sizeof(*renamed));
    if (!renamed) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    state->renamed = renamed;
    renamed[state->renamed_count++] = (uint32_t)at;
    heading->renamed = true;
    return true;
}

/* The heading's node shows the name in the pass's buffer unless `shown`,
 * `shown_size` bytes, which it shows, is that name; the bytes it showed go
 * as show_anchor says. The node's bytes, or NULL with the parse failed. */
static inline const unsigned char *show_name(name_pass *pass, const markdown_core_heading_parse *heading,
                                             const unsigned char *shown, bufsize_t shown_size, unsigned char **kept) {
    const markdown_core_strbuf *name = &pass->name;
    if (shown && shown_size == name->size && memcmp(shown, name->ptr, (size_t)name->size) == 0) {
        return shown;
    }
    markdown_core_chunk made = {name->ptr, name->size, 0};
    if (!markdown_core_chunk_to_cstr(&made)) {
        markdown_core_parser_fail(pass->parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    return show_anchor(heading->node, made, kept);
}

/* The heading's entry names `shown`, the name in the pass's buffer, of which
 * the first `base_size` bytes are its base. */
static inline void keep_name(name_pass *pass, markdown_core_heading_parse *heading, const unsigned char *shown,
                             bufsize_t base_size) {
    heading->name = shown;
    heading->name_size = pass->name.size;
    heading->base_size = base_size;
}

/* Heading `at` takes the name in the pass's buffer, of which the first
 * `base_size` bytes are its base, where its node and its entry part (A
 * COMMITTED HEADING UNDER ANOTHER NAME): an overlay names a committed
 * heading, whose entry keeps its committed name, or the node shows the name
 * a snapshot gave it, and its entry holds the committed one. The node's
 * bytes, or NULL with the parse failed. */
static const unsigned char *give_name_apart(name_pass *pass, size_t at, bufsize_t base_size) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_parse *heading = heading_at(state, at);
    const unsigned char *shown;
    if (heading->holds_name) {
        const markdown_core_chunk *anchor = &heading->node->attributes.anchor;
        if (!(shown = show_name(pass, heading, anchor->data, anchor->len, NULL))) {
            return NULL;
        }
    } else {
        unsigned char *kept = NULL;
        if (!(shown = show_name(pass, heading, heading->name, heading->name_size, &kept))) {
            return NULL;
        }
        assert(!kept || kept == heading->name);
        heading->holds_name = kept != NULL;
    }
    if (pass->beneath && at < state->named) {
        return rename_committed(parser, state, at) ? shown : NULL;
    }
    markdown_core_free((void *)heading->name);
    heading->holds_name = false;
    keep_name(pass, heading, shown, base_size);
    return shown;
}

/* The heading, whose node and entry agree, takes the name in the pass's
 * buffer, of which the first `base_size` bytes are its base, and its node
 * shows it, unless it shows it already: the entry names the bytes its node
 * shows. The node's bytes, or NULL with the parse failed. */
static inline const unsigned char *hand_name(name_pass *pass, markdown_core_heading_parse *heading,
                                             bufsize_t base_size) {
    const unsigned char *shown = show_name(pass, heading, heading->name, heading->name_size, NULL);
    if (shown) {
        keep_name(pass, heading, shown, base_size);
    }
    return shown;
}

/* The entry `entry`, vacant or held by a later heading, names `bytes`,
 * heading `at`'s name. That heading, as its place plus one, or zero. */
static inline size_t claim_name(name_pass *pass, size_t at, markdown_core_key_index_slot *entry,
                                const unsigned char *bytes) {
    const size_t holder = entry->key ? entry->extra : 0;
    if (entry->key) {
        entry->key = bytes;
    } else {
        markdown_core_key_index_commit(pass->index, entry, bytes);
        entry->value = 1;
    }
    entry->extra = (uint32_t)(at + 1);
    return holder;
}

/* The suffix of the name in the buffer, a name of a heading whose base is
 * its first `base_size` bytes: zero for the base. */
static size_t name_suffix(const markdown_core_strbuf *name, bufsize_t base_size) {
    size_t suffix = 0;
    for (bufsize_t i = base_size + 1; i < name->size; i++) {
        suffix = suffix * 10 + (size_t)(name->ptr[i] - '0');
    }
    return suffix;
}

/* Heading `at`, after every heading the pass named, takes the first name of
 * its sequence that is vacant: its base, the buffer's first `base_size`
 * bytes, then the base with the suffix the base's entry keeps, which it
 * advances, so a run of headings projecting to one name tries each suffix
 * once. Such a heading is not committed under an overlay, and holds no name:
 * its node and its entry agree. */
static void take_last(name_pass *pass, markdown_core_heading_parse *heading, size_t at, bufsize_t base_size) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_key_index *index = pass->index;
    markdown_core_strbuf *name = &pass->name;
    markdown_core_key_index_slot *projected = name_entry(pass, name->ptr, name->size);
    markdown_core_key_index_slot *candidate = projected;
    size_t capacity = index->capacity;
    while (candidate && candidate->key) {
        /* A lookup that adds an entry can move the base's: a vacant
         * candidate, which ends the search, and in an overlay a name it
         * takes from the committed index. */
        if (index->capacity != capacity) {
            projected = markdown_core_key_index_find(index, name->ptr, base_size);
            capacity = index->capacity;
        }
        markdown_core_strbuf_truncate(name, base_size);
        append_anchor_suffix(name, projected->value++);
        if (name->oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        candidate = name_entry(pass, name->ptr, name->size);
    }
    if (!candidate) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    assert(!heading->holds_name && !(pass->beneath && at < pass->state->named));
    const unsigned char *bytes = hand_name(pass, heading, base_size);
    if (bytes) {
        const size_t holder = claim_name(pass, at, candidate, bytes);
        assert(!holder);
        (void)holder;
    }
}

/* Heading `at`, displaced from the name in the buffer, takes the first name
 * past it in its sequence that is vacant or that a later heading holds, and
 * so on for each heading displaced in turn, each later than the last. */
static void displace(name_pass *pass, size_t at) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_strbuf *name = &pass->name;
    for (size_t holder = at + 1; holder && !parser->error;) {
        at = holder - 1;
        markdown_core_heading_parse *heading = heading_at(state, at);
        const bufsize_t base_size = heading->base_size;
        size_t suffix = name_suffix(name, base_size);
        markdown_core_key_index_slot *candidate;
        do {
            markdown_core_strbuf_truncate(name, base_size);
            append_anchor_suffix(name, ++suffix);
            candidate = name->oom ? NULL : name_entry(pass, name->ptr, name->size);
            if (!candidate) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return;
            }
        } while (candidate->key && (!candidate->extra || candidate->extra <= at + 1));
        const unsigned char *bytes = heading->holds_name || (pass->beneath && at < state->named)
                                         ? give_name_apart(pass, at, base_size)
                                         : hand_name(pass, heading, base_size);
        holder = bytes ? claim_name(pass, at, candidate, bytes) : 0;
    }
}

/* The record's name reserved: a vacant name becomes the record's, and one a
 * heading holds is taken from it, which goes on along its sequence. A
 * withdrawal that leaves no declaration of its name standing frees the
 * name: a declaration of it came before, which entered it. A pass that
 * starts empty reads neither a withdrawal nor what one withdrew, nor a
 * declaration an edit replaced or one its reading does not see, which an
 * edit's log holds where it was made (roster.h). */
static void read_record(name_pass *pass, const markdown_core_roster_record *record) {
    markdown_core_parser *parser = pass->parser;
    const markdown_core_chunk label = markdown_core_roster_label(record);
    markdown_core_key_index *index = pass->index;
    if (pass->fresh && !markdown_core_roster_seen(&parser->roster, record)) {
        return;
    }
    if (record->withdraws) {
        assert(!pass->beneath);
        if (!record->live) {
            pass->state->anchor_work += (size_t)label.len + 1;
            markdown_core_key_index_remove(index, markdown_core_key_index_find(index, label.data, label.len));
        }
        return;
    }
    markdown_core_key_index_slot *entry = name_entry(pass, label.data, label.len);
    if (!entry) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    if (!entry->key) {
        markdown_core_key_index_commit(index, entry, label.data);
        entry->value = 1;
        return;
    }
    if (!entry->extra) {
        return;
    }
    const size_t holder = entry->extra - 1;
    entry->key = label.data;
    entry->extra = 0;
    markdown_core_strbuf_clear(&pass->name);
    markdown_core_strbuf_put(&pass->name, label.data, label.len);
    if (pass->name.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    displace(pass, holder);
}

/* The cursor's records before position `end` of the log, reserved in log
 * order. */
static void read_records(name_pass *pass, markdown_core_roster_cursor *cursor, size_t end) {
    const markdown_core_roster *roster = &pass->parser->roster;
    for (const markdown_core_roster_record *record;
         !pass->parser->error && (record = markdown_core_roster_next_before(roster, cursor, end));) {
        read_record(pass, record);
    }
}

/* The headings from `first` to `end` that do not author their anchors
 * named, each after every heading the pass named before it. A heading whose
 * entry keeps the name it was given takes its base from it. */
static void name_headings(name_pass *pass, size_t first, size_t end) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_strbuf *name = &pass->name;
    for (size_t at = first; at < end && !parser->error; at++) {
        markdown_core_heading_parse *heading = heading_at(state, at);
        if (heading->authored) {
            continue;
        }
        markdown_core_strbuf_clear(name);
        if (heading->name) {
            state->anchor_work += (size_t)heading->base_size;
            markdown_core_strbuf_put(name, heading->name, heading->base_size);
            if (name->oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
        } else {
            heading_anchor_base(parser, state, heading->node, name, &pass->stack);
        }
        if (!parser->error) {
            take_last(pass, heading, at, name->size);
        }
    }
}

/* A committed heading the last snapshot renamed shows its committed name
 * again: the entry's bytes go back to its node. */
static void restore_name(markdown_core_heading_parse *heading) {
    const markdown_core_chunk name = {(unsigned char *)heading->name, heading->name_size, 1};
    show_anchor(heading->node, name, NULL);
    heading->holds_name = false;
}

/* A SNAPSHOT'S NAMES. The snapshot's tail -- the ANCHOR records its
 * completion made, then the headings it parsed, the pending block's copy
 * among them -- takes its names in an overlay over the committed names, which
 * stay as the commits left them. A reservation can displace committed
 * headings, which show the names the overlay gives them (A COMMITTED HEADING
 * UNDER ANOTHER NAME); then each committed heading the last snapshot renamed
 * and this one did not shows its committed name again, and the ones this one
 * renamed are the list. */
static void name_snapshot(name_pass *pass, markdown_core_roster_cursor cursor) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    const size_t total = heading_total(state);
    markdown_core_key_index overlay;
    if (!markdown_core_key_index_init(&overlay, cursor.left + total - state->named)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    pass->index = &overlay;
    pass->beneath = &state->taken;
    const size_t earlier = state->renamed_count;
    read_records(pass, &cursor, parser->roster.count);
    name_headings(pass, state->named, total);
    for (size_t i = 0; i < earlier && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, state->renamed[i]);
        if (heading->holds_name && !heading->renamed) {
            restore_name(heading);
        }
    }
    size_t count = 0;
    for (size_t i = earlier; i < state->renamed_count; i++) {
        heading_at(state, state->renamed[i])->renamed = false;
        state->renamed[count++] = state->renamed[i];
    }
    state->renamed_count = count;
    markdown_core_key_index_free(&overlay);
    pass->index = &state->taken;
    pass->beneath = NULL;
}

/* The committed names let go, for a pass that starts empty: a heading whose
 * entry holds its committed name frees it, and its entry names the bytes
 * its node shows. */
static void names_reset(markdown_core_heading_state *state) {
    for (size_t i = 0; i < state->renamed_count; i++) {
        markdown_core_heading_parse *heading = heading_at(state, state->renamed[i]);
        if (heading->holds_name) {
            const markdown_core_chunk *anchor = &heading->node->attributes.anchor;
            markdown_core_free((void *)heading->name);
            heading->holds_name = false;
            heading->name = anchor->data;
            heading->name_size = anchor->len;
        }
    }
    state->renamed_count = 0;
    markdown_core_key_index_free(&state->taken);
    state->records_at = state->records_read = state->named = 0;
}

/* A heading of a group being named again: where it starts, in the back's
 * terms when `back`, and its stem. */
typedef struct {
    uint32_t start, stem;
    bool back;
} stem_member;

static int stem_member_order(const void *a, const void *b) {
    const stem_member *left = a, *other = b;
    if (left->back != other->back) {
        return left->back ? 1 : -1;
    }
    /* The back's further from the end come first. */
    const int order = (left->start > other->start) - (left->start < other->start);
    return left->back ? -order : order;
}

/* The heading of the member, by halves among the headings. */
static markdown_core_heading_parse *stem_member_heading(markdown_core_heading_state *state, const stem_member *member) {
    markdown_core_heading_zipper *headings = &state->headings;
    const size_t at = member->back
                          ? headings->front.count + heading_zipper_back_from(headings, headings->end - member->start)
                          : heading_zipper_front_from(headings, member->start);
    markdown_core_heading_parse *heading = heading_at(state, at);
    assert(heading->start == member->start);
    return heading;
}

/* The stem joins the queue of a pass marked `mark` unless the pass saw it
 * or it holds no heading. False when storage could not be had. */
static bool stem_enqueue(markdown_core_heading_names *names, uint32_t **queue, size_t *count, size_t *room, uint32_t id,
                         uint32_t mark) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    if (stem->seen == mark || !stem_present(stem)) {
        return true;
    }
    uint32_t *grown = markdown_core_reserve(*queue, room, *count + 1, sizeof(**queue));
    if (!grown) {
        return false;
    }
    *queue = grown;
    stem->seen = mark;
    (*queue)[(*count)++] = id;
    return true;
}

/* The groups of the stems an edit touched, each a stem with its present
 * parent and children and theirs in turn, named again (THE NAMES AN EDIT
 * MOVES): their headings in order, each taking the first name of its
 * sequence that no anchor standing reserves and no heading of the groups
 * before it took, past the suffixes its base's earlier headings tried. A
 * heading whose name stays keeps its bytes. */
static void heading_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_names *names = state->names;
    const uint32_t mark = ++names->pass;
    uint32_t *queue = names->touched_count ? markdown_core_alloc(names->touched_count, sizeof(*queue)) : NULL;
    size_t count = 0, room = names->touched_count, members = 0;
    if (names->touched_count && !queue) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    for (size_t i = 0; i < names->touched_count; i++) {
        const uint32_t id = names->touched[i];
        markdown_core_heading_stem *stem = stem_of(names, id);
        if (stem->key && stem->seen != mark) {
            stem->seen = mark;
            queue[count++] = id;
        }
    }
    /* The group of each: a stem's present parent and children, and theirs. */
    for (size_t i = 0; i < count && !parser->error; i++) {
        const markdown_core_heading_stem *stem = stem_of(names, queue[i]);
        members += stem_zipper_count(&stem->starts);
        const bufsize_t size = stem_parent(stem->key, stem->size);
        const uint32_t parent = size ? stem_find(names, stem->key, size) : 0;
        bool ok = !parent || stem_enqueue(names, &queue, &count, &room, parent, mark);
        for (uint32_t next = stem->first; ok && next; next = stem_of(names, next)->next) {
            ok = stem_enqueue(names, &queue, &count, &room, next, mark);
        }
        if (!ok) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    stem_member *order = members ? markdown_core_alloc(members, sizeof(*order)) : NULL;
    markdown_core_key_index taken;
    if (parser->error || (members && (!order || !markdown_core_key_index_init(&taken, members)))) {
        markdown_core_free(order);
        markdown_core_free(queue);
        if (!parser->error) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        markdown_core_heading_stem *stem = stem_of(names, queue[i]);
        stem->suffix = 1;
        for (size_t k = 0; k < stem->starts.front.count; k++) {
            order[at++] = (stem_member){stem->starts.front.values[k], queue[i], false};
        }
        for (size_t k = 0; k < stem->starts.back_count; k++) {
            order[at++] = (stem_member){stem->starts.back[k], queue[i], true};
        }
    }
    markdown_core_free(queue);
    if (members > 1) {
        qsort(order, members, sizeof(*order), stem_member_order);
    }
    markdown_core_strbuf *name = &pass->name;
    for (size_t i = 0; i < members && !parser->error; i++) {
        markdown_core_heading_parse *heading = stem_member_heading(state, &order[i]);
        markdown_core_heading_stem *stem = stem_of(names, order[i].stem);
        const bufsize_t base = heading->base_size;
        markdown_core_strbuf_clear(name);
        markdown_core_strbuf_put(name, heading->name, base);
        for (;;) {
            bool failed = false;
            const bool reserved =
                markdown_core_roster_find(&parser->roster, MARKDOWN_CORE_ROSTER_ANCHOR, name->ptr, name->size, &failed);
            if (failed || name->oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
            if (!reserved && !markdown_core_key_index_find(&taken, name->ptr, name->size)) {
                break;
            }
            markdown_core_strbuf_truncate(name, base);
            append_anchor_suffix(name, stem->suffix++);
        }
        markdown_core_key_index_slot *slot =
            parser->error ? NULL : markdown_core_key_index_entry(&taken, name->ptr, name->size);
        const unsigned char *bytes = slot ? hand_name(pass, heading, base) : NULL;
        if (!bytes) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            break;
        }
        markdown_core_key_index_commit(&taken, slot, bytes);
    }
    if (members) {
        markdown_core_key_index_free(&taken);
    }
    markdown_core_free(order);
}

/* An edit's headings named (THE NAMES AN EDIT MOVES): each the reading
 * made takes its base and joins its stem, the stems an anchor it made or
 * let go names are touched with their parents, and the groups of the
 * stems touched are named again; then the stems with neither heading nor
 * child go. */
static void headings_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_names *names = state->names;
    markdown_core_strbuf *name = &pass->name;
    for (size_t i = names->fresh; i < state->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        if (heading->authored) {
            continue;
        }
        markdown_core_strbuf_clear(name);
        heading_anchor_base(parser, state, heading->node, name, &pass->stack);
        const unsigned char *bytes = parser->error ? NULL : hand_name(pass, heading, name->size);
        const uint32_t id = bytes ? stem_make(names, name->ptr, name->size) : 0;
        if (!id || !stem_add(names, id, heading->start)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    markdown_core_strbuf anchors = MARKDOWN_CORE_BUF_INIT();
    if (!parser->error && !markdown_core_roster_anchors(&parser->roster, names->rest, &anchors)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    for (bufsize_t at = 0; !parser->error && at < anchors.size;) {
        const bufsize_t size = (bufsize_t)strlen((const char *)anchors.ptr + at);
        const unsigned char *label = anchors.ptr + at;
        const bufsize_t parent = stem_parent(label, size);
        const uint32_t own = stem_find(names, label, size), up = parent ? stem_find(names, label, parent) : 0;
        if ((own && !stem_touch(names, own)) || (up && !stem_touch(names, up))) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        at += size + 1;
    }
    markdown_core_strbuf_free(&anchors);
    if (!parser->error) {
        heading_rename(pass);
    }
    for (size_t i = 0; i < names->touched_count; i++) {
        stem_release(names, names->touched[i]);
    }
    names->touched_count = 0;
    names->fresh = state->count;
}

/* Each heading's anchor (THE COMMITTED NAMES): an authored one stays, and a
 * computed one is the heading's own copy of the name chosen; only a heading
 * whose anchor changes is written. A snapshot moves the committed names on,
 * by the records and the headings its commit added before the oldest
 * candidate's mark, and names the rest over them, its tail among them; the
 * finalize names everything from an empty index. */
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    const bool snapshot = parser->snapshot;
    heading_sync(state);
    if (state->names && !snapshot) {
        name_pass pass = {.parser = parser, .state = state, .name = MARKDOWN_CORE_BUF_INIT()};
        if (!headings_drop_back(state)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        } else {
            headings_rename(&pass);
        }
        markdown_core_free(pass.stack.values);
        markdown_core_strbuf_free(&pass.name);
        return;
    }
    const size_t total = heading_total(state);
    size_t committed = total, committed_records = parser->roster.count;
    if (snapshot) {
        committed =
            ((const markdown_core_heading_state *)markdown_core_parser_kept(parser, self, &committed_records))->parsed;
    } else {
        names_reset(state);
    }
    if (!state->taken.size) {
        /* Nothing is named yet: the authored headings ask for no name. */
        while (state->named < committed && heading_at(state, state->named)->authored) {
            state->named++;
        }
        size_t first = state->named;
        while (first < total && heading_at(state, first)->authored) {
            first++;
        }
        if (first == total) {
            return;
        }
    }
    const markdown_core_roster *roster = &parser->roster;
    const size_t records = roster->spaces[MARKDOWN_CORE_ROSTER_ANCHOR].records;
    markdown_core_roster_cursor cursor = {MARKDOWN_CORE_ROSTER_ANCHOR, state->records_at,
                                          records - state->records_read};
    if (!state->taken.capacity && !markdown_core_key_index_init(&state->taken, total + cursor.left)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    name_pass pass;
    pass.parser = parser;
    pass.state = state;
    pass.index = &state->taken;
    pass.beneath = NULL;
    pass.fresh = !snapshot;
    pass.name = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT();
    pass.stack = (anchor_projection_stack){0};
    read_records(&pass, &cursor, snapshot ? committed_records : roster->count);
    state->records_at = cursor.at;
    state->records_read = records - cursor.left;
    name_headings(&pass, state->named, committed);
    state->named = committed;
    if (snapshot && !parser->error) {
        name_snapshot(&pass, cursor);
    }
    markdown_core_free(pass.stack.values);
    markdown_core_strbuf_free(&pass.name);
}

void markdown_core_prepare_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_heading_parse *heading) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, heading->node, true, &inline_state);
    while (!parser->error && !inline_state.error) {
        unsigned char c = markdown_core_inline_peek_char(&inline_state);
        /* Attribute ownership and opaque tokens are decided by the same
         * cursor as every inline. A live bracket makes this declaration
         * unwritable as a reference label; only its remaining inlines depend
         * on the document's completed symbol table. */
        if ((inline_state.last_delim && inline_state.last_delim->kind == DELIMITER_FIELD) ||
            (inline_state.pos != inline_state.text_end && inline_state.pos >= inline_state.opaque_end &&
             (c == '[' || c == ']' ||
              ((c == '!' || c == '^') && markdown_core_inline_peek_char_n(&inline_state, 1) == '[')))) {
            heading->pending = markdown_core_alloc(1, sizeof(inline_state));
            if (heading->pending) {
                *heading->pending = inline_state;
                return;
            }
            inline_state.error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            break;
        }
        if (markdown_core_inline_is_eof(&inline_state) || !markdown_core_inline_parse_inline(parser, &inline_state)) {
            break;
        }
    }
    if (!parser->error && !inline_state.error) {
        if (self->peers[HEADING_CITATION]) {
            markdown_core_citation_finish_run_tokens(self->peers[HEADING_CITATION], &inline_state);
        }
        markdown_core_inline_process_delimiters(parser, &inline_state, 0, NULL);
        const markdown_core_heading_run *run = markdown_core_run_state(&inline_state, self);
        markdown_core_chunk label = {inline_state.input.data, run->label_end, 0};
        /* A heading whose text is writable as a reference label declares
         * it, normalized: an occurrence naming it resolves to the heading
         * when no Reference declares it. It declares it once: a heading
         * parsed again has. */
        markdown_core_chunk *declared = &heading->node->as.heading->label;
        if (!declared->len && label.len > 0 && label.len <= MAX_LINK_LABEL_LENGTH &&
            markdown_core_inline_reference_label_length(label.data, label.len) == label.len) {
            if (!markdown_core_roster_normal_label(&parser->roster, parser->pool, &label, declared) ||
                !markdown_core_roster_declare(&parser->roster, MARKDOWN_CORE_ROSTER_REFERENCE, declared->data,
                                              declared->len, heading->node->place.start)) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
        }
    }
    markdown_core_inline_clear_inlines(&inline_state);
}

void markdown_core_finish_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    if (heading->pending) {
        markdown_core_inline_finish_inlines(parser, heading->pending);
        markdown_core_free(heading->pending);
        heading->pending = NULL;
    }
}

void markdown_core_dispose_heading(markdown_core_heading_parse *heading) {
    if (heading->pending) {
        markdown_core_inline_clear_inlines(heading->pending);
        markdown_core_free(heading->pending);
        heading->pending = NULL;
    }
}

/* (heading.h) The attribute tails the content's end may leave open on the
 * heading's last line, to the run's context sink: every brace that may be
 * one, since which is the tail only the finished line tells. */
void markdown_core_heading_report_tail(markdown_core_inline_state *inline_state, bool declares) {
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    bufsize_t at = len;
    while (at > 0 && !markdown_core_is_line_end(data[at - 1])) {
        at--;
    }
    markdown_core_field field;
    markdown_core_member_memo memo = {0};
    for (; at < len; at++) {
        if (data[at] == '\\' && at + 1 < len && markdown_core_ispunct(data[at + 1])) {
            at++;
        } else if (data[at] == '{' && markdown_core_attributes_open_member(data, len, at, &field, &memo)) {
            field.owner = MARKDOWN_CORE_KIND_HEADING;
            field.declares = declares;
            if (declares) {
                markdown_core_inline_open_line(inline_state, MARKDOWN_CORE_KIND_NONE, at, at + 1, &field, 1);
            } else {
                markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_NONE, at, at + 1, &field, 1);
            }
        }
    }
    markdown_core_member_memo_free(&memo);
}

void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    markdown_core_heading_run *run = markdown_core_run_state(inline_state, self);
    if (parent->kind == MARKDOWN_CORE_NODE_HEADING) {
        bufsize_t line = inline_state->input.len;
        inline_state->attributes = (markdown_core_attribute_parser){
            .data = inline_state->input.data, .length = inline_state->input.len, .scratch = &parser->attribute_scratch};
        /* Trailing attributes end the heading's last line with `}`. Only a
         * heading that does is walked back to that line's start, which is
         * where the attributes' opening brace is searched from. */
        run->attributes_start = -1;
        if (line > 0 && inline_state->input.data[line - 1] == '}') {
            while (line > 0 && !markdown_core_is_line_end(inline_state->input.data[line - 1])) {
                line--;
            }
            run->attributes_start =
                markdown_core_attributes_tail(&inline_state->attributes, line, inline_state->input.len);
        }
        if (run->attributes_start >= 0) {
            bufsize_t end = run->attributes_start;
            while (end > line && markdown_core_is_space_or_tab(inline_state->input.data[end - 1])) {
                end--;
            }
            if (!parent->as.heading->setext) {
                bufsize_t hashes = end;
                while (hashes > line && inline_state->input.data[hashes - 1] == '#') {
                    hashes--;
                }
                if (hashes < end &&
                    (hashes == line || markdown_core_is_space_or_tab(inline_state->input.data[hashes - 1]))) {
                    end = hashes;
                    while (end > line && markdown_core_is_space_or_tab(inline_state->input.data[end - 1])) {
                        end--;
                    }
                }
            }
            inline_state->text_end = end;
        } else if (!parent->as.heading->setext) {
            bufsize_t hashes = inline_state->input.len;
            while (hashes > 0 && inline_state->input.data[hashes - 1] == '#') {
                hashes--;
            }
            if (hashes < inline_state->input.len &&
                (hashes == 0 || markdown_core_is_space_or_tab(inline_state->input.data[hashes - 1]))) {
                inline_state->input.len = hashes;
                markdown_core_chunk_rtrim(&inline_state->input);
            }
        }
        if (inline_state->context && run->attributes_start < 0) {
            markdown_core_heading_report_tail(inline_state, false);
        }
        if (inline_state->attributes.oom) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }

    run->label_end = inline_state->input.len;
}

bool markdown_core_heading_claim_tail(const markdown_core_element_instance *self,
                                      markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    if (inline_state->pos == inline_state->text_end) {
        markdown_core_heading_run *run = markdown_core_run_state(inline_state, self);
        bufsize_t end;
        if (markdown_core_attributes_parse(&inline_state->attributes, run->attributes_start, &parent->attributes,
                                           &end)) {
            run->label_end = inline_state->text_end;
            inline_state->pos = inline_state->input.len;
        }
        if (inline_state->attributes.oom) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
        return true;
    }
    return false;
}

/* THE PARTS OF A HEADING (parts.h): the attribute tail that ends its last
 * content line, as markdown_core_heading_begin_inlines finds it, unless a
 * node of its content holds the brace. A setext heading's last content line
 * is the one before its underline. */
void markdown_core_heading_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    bufsize_t end = parts->length;
    while (end > 0 && markdown_core_is_line_end(s[end - 1])) {
        end--;
    }
    if (parts->node->as.heading->setext) {
        while (end > 0 && !markdown_core_is_line_end(s[end - 1])) {
            end--;
        }
        while (end > 0 && markdown_core_is_line_end(s[end - 1])) {
            end--;
        }
    }
    bufsize_t line = end;
    while (line > 0 && !markdown_core_is_line_end(s[line - 1])) {
        line--;
    }
    markdown_core_attributes_tail_parts(parts, line, end);
}

#define peek_at(input, at) ((input)->data[(at)])
static bool open_atx(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    bufsize_t hashpos;
    int level = 0;
    bufsize_t heading_startpos = parser->at.first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace + matched - parser->at.offset, false);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
    if (!*container) {
        return false;
    }

    hashpos = markdown_core_chunk_strchr(input, '#', parser->at.first_nonspace);

    while (peek_at(input, hashpos) == '#') {
        level++;
        hashpos++;
    }

    (*container)->as.heading->level = level;
    (*container)->as.heading->setext = false;
    (*container)->internal_offset = matched;

    return true;
}
static bool open_setext(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    bool has_content;
    // The paragraph's reference definitions are read first; what is left becomes the heading.
    markdown_core_node *parent = markdown_core_parser_parent(parser, *container);
    has_content = markdown_core_block_resolve_reference_link_definitions(parser, *container, parent);

    if (has_content) {

        markdown_core_node_set_kind_result result =
            markdown_core_parser_set_node_kind(parser, *container, parent, MARKDOWN_CORE_NODE_HEADING);
        if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
            if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            return false;
        }
        (*container)->as.heading->level = matched;
        (*container)->as.heading->setext = true;
        markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->at.offset, false);
    }

    return true;
}
static bool scan_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         block_start_context *context, block_start *start) {
    (void)self;
    if ((start->matched = scan_atx_heading_start(context->input->data, context->input->len, context->first))) {
        start->open = open_atx;
        if (parser->sink) {
            const unsigned char *data = context->input->data;
            bufsize_t end = context->first, last = context->input->len;
            while (end < last && data[end] == '#') {
                end++;
            }
            while (last > end && (data[last - 1] == '\n' || data[last - 1] == '\r')) {
                last--;
            }
            if (end == last) {
                markdown_core_topic_report_line(parser, context->first, end);
            }
        }
    } else if (context->paragraph &&
               (start->matched = scan_setext_heading_line(context->input->data, context->input->len, context->first))) {
        start->open = open_setext;
    } else {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HEADING;
    return true;
}
static int continue_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            unsigned char *data, int length, markdown_core_node *container) {
    return 0;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .peers = HEADING_PEERS,
    .begin_inline = markdown_core_heading_begin_inlines,
    .claim_inline_tail = markdown_core_heading_claim_tail,
    .reparse_inlines = reparse_inlines,

    .name = "heading",
    .maximum_block_indent = 3,
    .scan_block_start = scan_heading,
    /* ATX `#`; a setext underline of `=` or `-`. */
    .scan_block_gate = {.bytes = "#=-"},
    .last_block_matches = continue_heading,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inline_content = true,
    .deferred_inlines = true,
    .close_block = close_heading,
    .blank_opaque = true,
    .state_size = sizeof(markdown_core_heading_state),
    .kept_size = offsetof(markdown_core_heading_state, headings),
    .run_state_size = sizeof(markdown_core_heading_run),
};
