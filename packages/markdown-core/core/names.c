#include <assert.h>
#include <string.h>
#include "alloc.h"
#include "claim.h"
#include "element.h"
#include "names.h"
#include "parser.h"
#include "pieces.h"
#include "roster.h"

/* The entries' tree (names.h): an entry's one place is where its
 * node starts, and an entry with a number tells where it is. */
static size_t named_place(const void *entry) { return ((const markdown_core_named *)entry)->start; }

static void named_shift(void *value, int64_t delta) {
    markdown_core_named *entry = value;
    entry->start = (uint32_t)((int64_t)entry->start + delta);
}

static void named_moved(const markdown_core_tree *tree, void *entry, markdown_core_tree_leaf *leaf, uint32_t slot) {
    const uint32_t id = ((const markdown_core_named *)entry)->id;
    const markdown_core_names *state =
        (const markdown_core_names *)(const void *)((const char *)tree - offsetof(markdown_core_names, entries));
    if (id) {
        *markdown_core_array_write(&state->edit->where, id - 1) = (markdown_core_tree_handle){leaf, slot};
    }
}

static const markdown_core_tree_ops named_ops = {
    sizeof(markdown_core_named), named_place, NULL, named_shift, named_moved, NULL};

MARKDOWN_CORE_TREE_OPS(named_tree, markdown_core_named)

/* The entries' tree, made at its first use. */
static inline markdown_core_tree *entries_of(markdown_core_names *state) {
    if (!state->entries.ops) {
        state->entries = markdown_core_tree_make(&named_ops);
    }
    return &state->entries;
}

/* A number for an entry of an edit's parse, or 0 when storage could not
 * be had. */
static uint32_t named_number(markdown_core_name_edit *names) {
    if (names->free) {
        const uint32_t id = names->free;
        names->free = names->where.values[id - 1].slot;
        return id;
    }
    if (!markdown_core_array_push(&names->where, ((markdown_core_tree_handle){NULL, 0}))) {
        return 0;
    }
    return (uint32_t)names->where.count;
}

/* The entries from rank `index`, `count` of them, go, with their
 * numbers. */
static void entries_erase(markdown_core_names *state, size_t index, size_t count) {
    markdown_core_tree *entries = entries_of(state);
    for (size_t i = index; state->edit && i < index + count; i++) {
        const uint32_t id = named_tree_at(entries, i)->id;
        if (id) {
            *markdown_core_array_write(&state->edit->where, id - 1) =
                (markdown_core_tree_handle){NULL, state->edit->free};
            state->edit->free = id;
        }
    }
    markdown_core_tree_erase(entries, index, count);
}

/* The front, which holds `count` of the reading's entries: a reading put
 * back to a mark left the entries past it (names.h). */
static inline void names_follow(markdown_core_names *state) {
    markdown_core_tree *entries = entries_of(state);
    if (entries->front > state->count) {
        if (state->edit) {
            entries_erase(state, state->count, entries->front - state->count);
        } else {
            markdown_core_tree_erase(entries, state->count, entries->front - state->count);
        }
    }
}

/* How many entries the parse holds, the old tail's it took back among
 * them, and entry `at` of them in source order, to change in place but
 * for where it starts. */
static inline size_t names_total(const markdown_core_names *state) {
    return state->count + (state->spliced ? state->entries.count - state->entries.front : 0);
}
static inline markdown_core_named *named_at(markdown_core_names *state, size_t at) {
    return named_tree_at(entries_of(state), at);
}

/* The entries from rank `first` to `count` in place, when one stretch of
 * storage holds them all, or NULL; and entry `at` of them, from the
 * stretch when there is one. */
static inline markdown_core_named *named_span(markdown_core_names *state, size_t first, size_t count) {
    size_t run = 0;
    markdown_core_named *span =
        first < count ? markdown_core_tree_run(entries_of(state), first, sizeof(*span), &run) : NULL;
    return run >= count - first ? span : NULL;
}
static inline markdown_core_named *held_at(markdown_core_names *state, markdown_core_named *span, size_t first,
                                           size_t at) {
    return span ? span + (at - first) : named_at(state, at);
}

static uint64_t named_start_key(const void *entry) { return ((const markdown_core_named *)entry)->start; }

/* The front's entries from `first` up to `count` put in source order:
 * taken out and put back in order when they are not. False when storage
 * could not be had. */
static bool entries_order(markdown_core_names *state, markdown_core_parser *parser, size_t first, size_t count) {
    markdown_core_tree *entries = entries_of(state);
    markdown_core_named *const span = named_span(state, first, count);
    bool ordered = true;
    for (size_t i = first + 1; ordered && i < count; i++) {
        ordered = held_at(state, span, first, i - 1)->start <= held_at(state, span, first, i)->start;
    }
    if (ordered) {
        return true;
    }
    markdown_core_named *taken = markdown_core_alloc(count - first, sizeof(*taken));
    if (!taken) {
        return false;
    }
    for (size_t i = first; i < count; i++) {
        taken[i - first] = *named_tree_at(entries, i);
    }
    bool ok = markdown_core_order_source_entries(&parser->source_order, taken, count - first, sizeof(*taken),
                                                 named_start_key);
    if (ok) {
        markdown_core_tree_erase(entries, first, count - first);
        for (size_t i = first; ok && i < count; i++) {
            ok = named_tree_insert(entries, i, taken[i - first], true) != NULL;
        }
    }
    markdown_core_free(taken);
    return ok;
}

/* The read entry that starts at `start`, found by halves among those in
 * source order. */
static markdown_core_named *parsed_named(markdown_core_names *state, uint32_t start) {
    names_follow(state);
    const size_t low = markdown_core_tree_from(entries_of(state), start, 0, state->parsed);
    assert(low < state->parsed && named_at(state, low)->start == start);
    return named_at(state, low);
}

/* THE NAMES. An entry's name is its base, the projection of its text
 * (names.h, the owner's `project`), or the base, a hyphen and a
 * suffix, a number (anchors.md); its node keeps the base and the suffix it shows (node.h,
 * A NODE'S NAME), and its anchor is their bytes. A name is read in
 * pieces (pieces.h): the base's, one or two, then the suffix's digits, so
 * no name is joined to be looked up, and its measure, by which the indexes
 * of names file it (map.h, KEYS UNDER A HASH THEIR USER GIVES), is the
 * base's joined with the suffix's. Two names are the same when their bytes
 * are: names whose measures agree are compared, piece by piece, and a
 * stretch both read where one buffer keeps it is not read at all. */
enum { SUFFIX_ROOM = 3 * sizeof(size_t) + 2 };

/* The mark of an index slot whose key is not bytes the slot points at: a
 * name an entry holds, a stem, a count of moves. */
static const unsigned char HELD[1] = {0};

/* The suffix `suffix` as a name ends with it, a hyphen and its digits, in
 * `tail`: how many bytes. */
static inline bufsize_t suffix_write(unsigned char *tail, size_t suffix) {
    bufsize_t size = 2;
    for (size_t rest = suffix / 10; rest; rest /= 10) {
        size++;
    }
    tail[0] = '-';
    for (bufsize_t at = size; at-- > 1; suffix /= 10) {
        tail[at] = (unsigned char)('0' + suffix % 10);
    }
    return size;
}

/* The measure of the bytes `pieces` holds. */
static inline markdown_core_measure pieces_measure(const markdown_core_pieces *pieces) {
    if (pieces->count == 1) {
        return markdown_core_measure_of(pieces->data[0], (size_t)pieces->sizes[0]);
    }
    markdown_core_measure measure = MARKDOWN_CORE_MEASURE_NONE;
    for (uint32_t i = 0; i < pieces->count; i++) {
        markdown_core_measure_add(&measure, pieces->data[i], (size_t)pieces->sizes[i]);
    }
    return measure;
}

/* A NAME IN PIECES: the base's pieces and its suffix's digits; and the
 * base's pieces and size, so that the name of the base and another suffix
 * is made in place. It reads its own digits, so it stays where it is made.
 * An edit, which must not read a long base whole, knows a name by its
 * measure, made of the base's (THE NAMES AN EDIT MOVES); the committed
 * pass, which reads each base whole, by the hash of its bytes read in turn
 * (map.h, A KEY READ IN PARTS), the base's read once for all its suffixes:
 * a name `hashed` keeps the hash, any other the measure. */
typedef struct {
    markdown_core_pieces pieces;
    markdown_core_measure measure, base;
    markdown_core_key_stream stream;
    uint64_t hash;
    bool hashed;
    uint32_t parts;
    bufsize_t size;
    size_t suffix;
    unsigned char tail[SUFFIX_ROOM];
} name_view;

/* The name of the base `base`, which measures `measure`. */
static inline void name_base(name_view *name, const markdown_core_pieces *base, markdown_core_measure measure) {
    name->pieces = *base;
    name->measure = name->base = measure;
    name->hashed = false;
    name->parts = base->count;
    name->size = base->size;
    name->suffix = 0;
}

/* The name of the base `base` pieces hold, hashed. */
static inline void name_hashed(name_view *name) {
    markdown_core_key_stream_start(&name->stream);
    for (uint32_t i = 0; i < name->pieces.count; i++) {
        markdown_core_key_stream_add(&name->stream, name->pieces.data[i], (size_t)name->pieces.sizes[i]);
    }
    name->hash = markdown_core_key_stream_end(&name->stream);
    name->hashed = true;
    name->parts = name->pieces.count;
    name->size = name->pieces.size;
    name->suffix = 0;
}

/* The name of its base and `suffix`. */
static inline void name_suffix(name_view *name, size_t suffix) {
    name->pieces.count = name->parts;
    name->pieces.size = name->size;
    name->suffix = suffix;
    const bufsize_t size = suffix ? suffix_write(name->tail, suffix) : 0;
    if (size) {
        markdown_core_pieces_add(&name->pieces, name->tail, size);
    }
    if (name->hashed) {
        markdown_core_key_stream stream = name->stream;
        markdown_core_key_stream_add(&stream, name->tail, (size_t)size);
        name->hash = markdown_core_key_stream_end(&stream);
    } else {
        name->measure = size
                            ? markdown_core_measure_join(name->base, markdown_core_measure_of(name->tail, (size_t)size))
                            : name->base;
    }
}

/* The name of the base `base`, which measures `measure`, and `suffix`. */
static inline void name_make(name_view *name, const markdown_core_pieces *base, markdown_core_measure measure,
                             size_t suffix) {
    name_base(name, base, measure);
    name_suffix(name, suffix);
}

/* The base of a node whose text projects to nothing, which its node
 * borrows. */
static const markdown_core_strbuf SECTION = {.ptr = (unsigned char *)"section", .size = 7};

/* Where the node, one of the element's, keeps its name (names.h, the
 * owner's `value`). */
static inline markdown_core_name *value_of(const markdown_core_element *element, const markdown_core_node *node) {
    return element->names->value(node);
}

/* Where the entry's node keeps its name. */
static inline markdown_core_name *name_of(const markdown_core_names *state, const markdown_core_named *entry) {
    return value_of(state->owners[entry->owner]->element, entry->node);
}

/* The base the name keeps, in `*pieces`. */
static inline void value_base(const markdown_core_name *value, markdown_core_pieces *pieces) {
    markdown_core_pieces_clear(pieces);
    markdown_core_pieces_span(pieces, &value->base, 0, value->base.size);
}

/* The name of the base `value` keeps, hashed. */
static inline void name_value(name_view *name, const markdown_core_name *value) {
    value_base(value, &name->pieces);
    name_hashed(name);
}

/* The anchor the name showed, once read, goes: the name changed. */
static inline void name_unshow(markdown_core_name *value) {
    if (value->shown) {
        markdown_core_free(value->shown);
        value->shown = NULL;
    }
}

/* The name shows its base and `suffix`. */
static inline void name_show(markdown_core_name *value, uint32_t suffix) {
    if (value->suffix != suffix) {
        value->suffix = suffix;
        name_unshow(value);
    }
}

/* The name keeps no base. */
static void base_drop(markdown_core_name *value) {
    if (!markdown_core_strbuf_borrows(&value->base)) {
        markdown_core_strbuf_free(&value->base);
    }
    markdown_core_strbuf_init(&value->base, 0);
    value->suffix = 0;
    name_unshow(value);
}

/* THE NAMES AN EDIT MOVES. An entry of base p takes p or p-k (anchors.md),
 * so two names bear on each other only when one is the other with one
 * suffix `-k` more: p-k is q-j for no q other than p. A stem's parent is
 * its name less that suffix, and a stem with entries, a present one, and
 * its parent, when present too, are one group: the names of a group are
 * the names a pass over its entries alone gives them, in order, past the
 * anchors that stand, whatever the other groups hold. An edit names again
 * only the groups of the stems it touched: the bases of the entries its
 * reading let go and of those it read, and the stems an anchor it made or
 * let go names, with their parents (kin_rename), and of a group only
 * the entries from the rest point on, until the names behind it are the
 * names they were: so an edit beside one of many entries of one name
 * names few of them. Each stem keeps its entries in order, by their
 * numbers, so that a group's entries come in order from its stems, each
 * where its number says, and who holds a name is found by halves
 * (name_held). A name is a stem and a suffix, and an entry names its stem: what the edit lets go
 * and what a pass moves are those numbers, and no name is joined.
 *
 * A stem keeps no bytes: its witness reads them (names.h, THE STEMS),
 * and the index of the stems files each by its measure and compares the
 * bytes of its witness. Every witness holds until the edit's end: the bases
 * of the entries it read stay as they were until then (THE NEW BASES), the
 * nodes it let go live until the next publish does, and the base of a node
 * it made a kind that asks no name is kept aside
 * (markdown_core_names_retire). A stem that loses its last entry or child is touched, so that at the edit's end it
 * goes unless it holds one again.
 *
 * The first reopen makes the stems; a parse no edit reopened names its
 * entries as THE COMMITTED NAMES say. */
static const markdown_core_tree_ops stem_ops = {sizeof(uint32_t), NULL, NULL, NULL, NULL, NULL};

MARKDOWN_CORE_TREE_OPS(stem_tree, uint32_t)

static inline markdown_core_name_stem *stem_of(const markdown_core_name_edit *names, uint32_t id) {
    return &names->stems[id - 1];
}

static inline bool stem_present(const markdown_core_name_stem *stem) { return stem->members.count != 0; }

/* The base the entry's node had before the edit: its own, or the one the
 * edit kept aside as it made the node a kind that asks no name
 * (markdown_core_names_retire). */
static const markdown_core_strbuf *old_base(const markdown_core_names *state, const markdown_core_named *entry) {
    const markdown_core_name_edit *names = state->edit;
    for (size_t i = 0; i < names->retired_count; i++) {
        if (names->retired[i].node == entry->node) {
            return names->retired[i].base;
        }
    }
    return &name_of(state, entry)->base;
}

/* The bytes of a new base (THE NEW BASES), in `*pieces`. */
static void base_pieces(const markdown_core_name_base *base, markdown_core_pieces *pieces) {
    markdown_core_pieces_clear(pieces);
    if (base->old) {
        markdown_core_pieces_span(pieces, base->old, 0, base->front);
    }
    markdown_core_pieces_add(pieces, base->mid.ptr, base->mid.size);
    if (base->old) {
        markdown_core_pieces_span(pieces, base->old, base->old->size - base->back, base->old->size);
    }
}

/* Whether entry `at` is one the edit's reading made, whose base waits for
 * the edit's end (THE NEW BASES). */
static inline bool made_at(const markdown_core_names *state, size_t at) {
    return at >= state->edit->fresh && at < state->entries.front;
}

/* The base entry `at` is named with as the edit names: the new one of a
 * entry the reading made, the old one of any other. */
static void member_base(const markdown_core_names *state, size_t at, const markdown_core_named *entry,
                        markdown_core_pieces *pieces) {
    const markdown_core_name_edit *names = state->edit;
    if (made_at(state, at)) {
        base_pieces(&names->bases[at - names->fresh], pieces);
        return;
    }
    const markdown_core_strbuf *base = old_base(state, entry);
    markdown_core_pieces_clear(pieces);
    markdown_core_pieces_span(pieces, base, 0, base->size);
}

/* Where the stem's entry `i` has its entry. */
static inline markdown_core_tree_handle stem_where(const markdown_core_names *state,
                                                   const markdown_core_name_stem *stem, size_t i) {
    return state->edit->where.values[*stem_tree_at((markdown_core_tree *)&stem->members, i) - 1];
}

/* Where the stem's entry `i` starts in the text. */
static size_t stem_place_of(const markdown_core_names *state, const markdown_core_name_stem *stem, size_t i) {
    markdown_core_named entry;
    markdown_core_tree_read(&state->entries, stem_where(state, stem, i), &entry);
    return entry.start;
}

/* How many of the stem's entries start before `place`, by halves. */
static size_t stem_before(const markdown_core_names *state, const markdown_core_name_stem *stem, size_t place) {
    size_t low = 0, high = stem->members.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (stem_place_of(state, stem, middle) < place) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* The stem's entry `i`, by halves among the entries, and its place
 * among them in `*at`. */
static markdown_core_named *stem_member(markdown_core_names *state, const markdown_core_name_stem *stem, size_t i,
                                        size_t *at) {
    *at = markdown_core_tree_rank(&state->entries, stem_where(state, stem, i));
    return named_at(state, *at);
}

/* The stem's bytes, as its witness reads them (THE STEMS), in `*text`. */
static void stem_text(markdown_core_names *state, uint32_t id, markdown_core_pieces *text) {
    const markdown_core_name_stem *stem = stem_of(state->edit, id);
    if (stem_present(stem)) {
        size_t at;
        const markdown_core_named *entry = stem_member(state, stem, 0, &at);
        member_base(state, at, entry, text);
        return;
    }
    if (stem->first || stem->heir) {
        markdown_core_pieces whole;
        stem_text(state, stem->first ? stem->first : stem->heir, &whole);
        markdown_core_pieces_head(&whole, (bufsize_t)stem->measure.size, text);
        return;
    }
    markdown_core_pieces_clear(text);
    markdown_core_pieces_span(text, stem->relic, 0, (bufsize_t)stem->measure.size);
}

/* What the index of the stems compares: the bytes looked for. */
typedef struct {
    markdown_core_names *state;
    const markdown_core_pieces *text;
} stem_probe;

static bool stem_holds(const void *probe, const markdown_core_key_index_slot *slot) {
    const stem_probe *look = probe;
    markdown_core_pieces text;
    stem_text(look->state, (uint32_t)slot->value, &text);
    return markdown_core_pieces_same(look->text, &text);
}

/* The stem of the name `text`, which measures `measure`: its place plus
 * one, or zero when none is kept. */
static uint32_t stem_find(markdown_core_names *state, const markdown_core_pieces *text, markdown_core_measure measure) {
    const stem_probe probe = {state, text};
    const markdown_core_key_index_slot *slot = markdown_core_key_index_seek(
        &state->edit->index, markdown_core_key_index_hash(measure), (bufsize_t)measure.size, stem_holds, &probe);
    return slot ? (uint32_t)slot->value : 0;
}

/* The stem of the name, made when none is kept, which takes a witness at
 * once: an entry or a child. Zero when storage could not be had. */
static uint32_t stem_make(markdown_core_names *state, const markdown_core_pieces *text, markdown_core_measure measure) {
    markdown_core_name_edit *names = state->edit;
    const stem_probe probe = {state, text};
    markdown_core_key_index_slot *slot = markdown_core_key_index_place(
        &names->index, markdown_core_key_index_hash(measure), (bufsize_t)measure.size, stem_holds, &probe);
    if (!slot) {
        return 0;
    }
    if (slot->key) {
        return (uint32_t)slot->value;
    }
    uint32_t id = names->spare;
    if (id) {
        names->spare = stem_of(names, id)->next;
    } else {
        markdown_core_name_stem *stems =
            markdown_core_reserve(names->stems, &names->stem_room, names->stem_count + 1, sizeof(*stems));
        if (!stems) {
            return 0;
        }
        names->stems = stems;
        id = (uint32_t)++names->stem_count;
    }
    *stem_of(names, id) = (markdown_core_name_stem){.members = markdown_core_tree_make(&stem_ops), .measure = measure};
    markdown_core_key_index_commit(&names->index, slot, HELD);
    slot->value = id;
    return id;
}

/* The stem's slot, found by its place: the stem may read no bytes then. */
static bool stem_is(const void *probe, const markdown_core_key_index_slot *slot) {
    return (uint32_t)slot->value == *(const uint32_t *)probe;
}

/* THE PARENT of a name `name`, which measures `measure`: the name less a
 * last suffix `-k`, k a number of no leading zero that a suffix can be,
 * when some name is left; its measure in `*parent` and k in `*suffix`.
 * False when it has none. */
static bool name_parent(const markdown_core_pieces *name, markdown_core_measure measure, markdown_core_measure *parent,
                        uint32_t *suffix) {
    unsigned char tail[12];
    const bufsize_t count = markdown_core_pieces_tail(name, (bufsize_t)sizeof(tail), tail);
    bufsize_t at = count;
    while (at > 0 && tail[at - 1] >= '0' && tail[at - 1] <= '9') {
        at--;
    }
    const bufsize_t width = count - at;
    if (!width || width > 10 || !at || tail[at - 1] != '-' || tail[at] == '0' || name->size - width < 2) {
        return false;
    }
    uint64_t value = 0;
    for (bufsize_t i = at; i < count; i++) {
        value = value * 10 + (uint64_t)(tail[i] - '0');
    }
    if (value > UINT32_MAX) {
        return false;
    }
    *parent = markdown_core_measure_front(markdown_core_measure_of(tail + at - 1, (size_t)width + 1), measure);
    *suffix = (uint32_t)value;
    return true;
}

/* The parent of the stem `id`, its place plus one, or zero when none is
 * kept; the suffix that parts them in `*suffix`. */
static uint32_t stem_parent(markdown_core_names *state, uint32_t id, uint32_t *suffix) {
    const markdown_core_name_stem *stem = stem_of(state->edit, id);
    markdown_core_pieces text;
    stem_text(state, id, &text);
    markdown_core_measure measure;
    if (!name_parent(&text, stem->measure, &measure, suffix)) {
        return 0;
    }
    if (stem->up) {
        return stem->up;
    }
    markdown_core_pieces head;
    markdown_core_pieces_head(&text, (bufsize_t)measure.size, &head);
    return stem_find(state, &head, measure);
}

/* The stem whose base is the name of the stem `id` and `suffix`: the stem
 * itself for no suffix. Zero when none is kept. */
static uint32_t name_stem(markdown_core_names *state, uint32_t id, uint32_t suffix) {
    if (!suffix) {
        return id;
    }
    markdown_core_pieces text;
    stem_text(state, id, &text);
    name_view name;
    name_make(&name, &text, stem_of(state->edit, id)->measure, suffix);
    return stem_find(state, &name.pieces, name.measure);
}

/* The stem names again once the edit's reading is done. False when storage
 * could not be had. */
static bool stem_touch(markdown_core_name_edit *names, uint32_t id) {
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
static bool stem_link(markdown_core_names *state, uint32_t id) {
    markdown_core_name_edit *names = state->edit;
    markdown_core_pieces text;
    stem_text(state, id, &text);
    markdown_core_measure measure;
    uint32_t suffix;
    if (!name_parent(&text, stem_of(names, id)->measure, &measure, &suffix)) {
        return true;
    }
    markdown_core_pieces head;
    markdown_core_pieces_head(&text, (bufsize_t)measure.size, &head);
    const uint32_t parent = stem_make(state, &head, measure);
    if (!parent || !stem_touch(names, parent)) {
        return false;
    }
    markdown_core_name_stem *own = stem_of(names, id), *up = stem_of(names, parent);
    own->up = parent;
    own->prior = 0;
    own->next = up->first;
    if (up->first) {
        stem_of(names, up->first)->prior = id;
    }
    up->first = id;
    return true;
}

/* The stem, no longer present, leaves its parent's children: the parent
 * that keeps none then reads its bytes from it (THE STEMS), and is touched.
 * False when storage could not be had. */
static bool stem_unlink(markdown_core_name_edit *names, uint32_t id) {
    markdown_core_name_stem *stem = stem_of(names, id);
    const uint32_t parent = stem->up;
    if (!parent) {
        return true;
    }
    markdown_core_name_stem *up = stem_of(names, parent);
    if (stem->prior) {
        stem_of(names, stem->prior)->next = stem->next;
    } else {
        up->first = stem->next;
    }
    if (stem->next) {
        stem_of(names, stem->next)->prior = stem->prior;
    }
    stem->next = stem->prior = stem->up = 0;
    if (!up->first) {
        up->heir = id;
        up->relic = NULL;
    }
    return stem_touch(names, parent);
}

/* The stem, with no entry and no child, goes, its place then free. */
static void stem_release(markdown_core_name_edit *names, uint32_t id) {
    markdown_core_name_stem *stem = stem_of(names, id);
    if (!stem->measure.size || stem_present(stem) || stem->first) {
        return;
    }
    markdown_core_key_index_remove(
        &names->index, markdown_core_key_index_seek(&names->index, markdown_core_key_index_hash(stem->measure),
                                                    (bufsize_t)stem->measure.size, stem_is, &id));
    markdown_core_tree_free(&stem->members);
    *stem = (markdown_core_name_stem){.next = names->spare};
    names->spare = id;
}

/* How many of the stem's entries come before entry `rank` of the
 * entries, by halves. */
static size_t stem_rank(const markdown_core_names *state, const markdown_core_name_stem *stem, size_t rank) {
    size_t low = 0, high = stem->members.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (markdown_core_tree_rank(&state->entries, stem_where(state, stem, middle)) < rank) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* The entry numbered `entry` is one of
 * stem `id`'s, in its order. False when storage could not be had. */
static bool stem_add(markdown_core_names *state, uint32_t id, uint32_t entry) {
    markdown_core_name_edit *names = state->edit;
    markdown_core_name_stem *stem = stem_of(names, id);
    const bool present = stem_present(stem);
    const size_t rank = markdown_core_tree_rank(&state->entries, names->where.values[entry - 1]);
    if (!stem_tree_insert(&stem->members, stem_rank(state, stem, rank), entry, false)) {
        return false;
    }
    return (present || stem_link(state, id)) && stem_touch(names, id);
}

/* Stem `id`'s entry numbered `entry` goes from it. False when storage
 * could not be had. */
static bool stem_drop(markdown_core_names *state, uint32_t id, uint32_t entry) {
    markdown_core_name_edit *names = state->edit;
    markdown_core_name_stem *stem = stem_of(names, id);
    const size_t at = stem_rank(state, stem, markdown_core_tree_rank(&state->entries, names->where.values[entry - 1]));
    assert(at < stem->members.count && *stem_tree_at(&stem->members, at) == entry);
    markdown_core_tree_erase(&stem->members, at, 1);
    return (stem_present(stem) || stem_unlink(names, id)) && stem_touch(names, id);
}

/* The entries' names, kept from now on as stems (THE NAMES AN EDIT MOVES):
 * a flushed parse named every entry that does not author its anchor, and
 * the front holds them all. False when storage could not be had. */
static bool stems_make(markdown_core_names *state) {
    markdown_core_name_edit *names = markdown_core_alloc(1, sizeof(*names));
    if (!names || !markdown_core_key_index_init(&names->index, state->count)) {
        markdown_core_free(names);
        return false;
    }
    state->edit = names;
    names->fresh = state->count;
    markdown_core_tree *entries = entries_of(state);
    /* Every entry numbered, where it stands: an append in place tells
     * nothing (tree.h). */
    for (size_t i = 0; i < state->count; i++) {
        named_tree_at(entries, i)->id = named_number(names);
        if (!named_tree_at(entries, i)->id) {
            return false;
        }
        *markdown_core_array_write(&names->where, named_tree_at(entries, i)->id - 1) =
            markdown_core_tree_where(entries, i);
    }
    for (size_t i = 0; i < state->count; i++) {
        markdown_core_named *entry = named_tree_at(entries, i);
        entry->stem = 0;
        if (entry->authored) {
            continue;
        }
        markdown_core_pieces base;
        value_base(name_of(state, entry), &base);
        const uint32_t id = stem_make(state, &base, pieces_measure(&base));
        if (!id || !stem_add(state, id, named_tree_at(entries, i)->id)) {
            return false;
        }
        named_tree_at(entries, i)->stem = id;
    }
    /* What the stems touched names nothing again: every name holds. */
    names->touched_count = 0;
    markdown_core_key_index_free(&state->taken);
    return true;
}

/* The new bases the last edit left, which its end gave the nodes or which
 * a failed edit leaves, and the bases it kept aside, let go. */
static void bases_free(markdown_core_name_edit *names) {
    for (size_t i = 0; i < names->base_count; i++) {
        markdown_core_strbuf_free(&names->bases[i].mid);
    }
    names->base_count = 0;
    for (size_t i = 0; i < names->retired_count; i++) {
        markdown_core_strbuf *base = names->retired[i].base;
        if (!markdown_core_strbuf_borrows(base)) {
            markdown_core_strbuf_free(base);
        }
        markdown_core_free(base);
    }
    names->retired_count = 0;
}

static void stems_free(markdown_core_name_edit *names) {
    if (!names) {
        return;
    }
    for (size_t i = 0; i < names->stem_count; i++) {
        markdown_core_tree_free(&names->stems[i].members);
    }
    markdown_core_array_free(&names->where);
    bases_free(names);
    markdown_core_free(names->stems);
    markdown_core_key_index_free(&names->index);
    markdown_core_free(names->touched);
    markdown_core_free(names->lost);
    markdown_core_free(names->bases);
    markdown_core_free(names->retired);
    markdown_core_free(names);
}

/* A flushed parse names every entry and parses each, so its entries
 * are in source order: the focus moves to `start`, those from the first
 * that starts there or past it going to the back, each start measured back
 * from the text's end (THE BACK, blocks.c), and the back's before it coming
 * to the front. */
bool markdown_core_names_reopen(markdown_core_parser *parser, uint32_t start) {
    markdown_core_names *state = &parser->names;
    names_follow(state);
    markdown_core_tree *entries = &state->entries;
    assert(state->parsed == state->count && (state->spliced || entries->front == entries->count));
    if (!state->edit && !stems_make(state)) {
        return false;
    }
    state->end = markdown_core_parser_back_end(parser);
    const bool back = entries->front < entries->count && named_tree_at(entries, entries->front)->start < start;
    markdown_core_tree_focus(entries, back ? markdown_core_tree_from(entries, start, entries->front, entries->count)
                                           : markdown_core_tree_from(entries, start, 0, entries->front));
    const size_t low = entries->front;
    state->edit->rest = start;
    state->edit->fresh = low;
    state->edit->lost_count = 0;
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

/* The entry, which the edit lets go, of stem `id`, its name noted as its
 * stem and suffix (kin_rename), its base the stem's witness while the
 * stem holds no other, and gone from the stem's entries. False when
 * storage could not be had. */
static bool stem_lost(markdown_core_names *state, uint32_t id, const markdown_core_named *entry) {
    markdown_core_name_edit *names = state->edit;
    markdown_core_name_lost *lost =
        markdown_core_reserve(names->lost, &names->lost_room, names->lost_count + 1, sizeof(*lost));
    if (!lost) {
        return false;
    }
    names->lost = lost;
    lost[names->lost_count++] = (markdown_core_name_lost){id, entry->suffix};
    markdown_core_name_stem *stem = stem_of(names, id);
    stem->relic = old_base(state, entry);
    stem->heir = 0;
    return stem_drop(state, id, entry->id);
}

/* The back's entries, from the focus, that start before `point` of the
 * new text go, the reading's own read again: the rest are the old tail the
 * reading takes back, which move with the edit. */
bool markdown_core_names_splice(markdown_core_parser *parser, uint32_t point) {
    markdown_core_names *state = &parser->names;
    names_follow(state);
    markdown_core_tree *entries = &state->entries;
    /* The back's are cut in the old text's terms, the end the reopen
     * measured them from: those before the edit moved with it. */
    const int64_t delta = (int64_t)markdown_core_parser_back_end(parser) - (int64_t)state->end;
    const size_t old = (size_t)((int64_t)point - delta);
    const size_t gone = markdown_core_tree_from(entries, old, entries->front, entries->count) - entries->front;
    for (size_t k = 0; k < gone; k++) {
        const markdown_core_named *entry = named_tree_at(entries, entries->front + k);
        if (entry->stem && !stem_lost(state, entry->stem, entry)) {
            return false;
        }
    }
    entries_erase(state, entries->front, gone);
    markdown_core_tree_shift(entries, entries->front, delta);
    state->end = markdown_core_parser_back_end(parser);
    state->spliced = true;
    return true;
}

/* The old tail a reading did not take back goes: it read every line. */
static bool back_drop(markdown_core_names *state) {
    markdown_core_tree *entries = entries_of(state);
    if (state->spliced || entries->front == entries->count) {
        return true;
    }
    for (size_t i = entries->front; i < entries->count; i++) {
        const markdown_core_named *entry = named_tree_at(entries, i);
        if (entry->stem && !stem_lost(state, entry->stem, entry)) {
            return false;
        }
    }
    entries_erase(state, entries->front, entries->count - entries->front);
    return true;
}

/* Release the entries and the names they took. The anchors are assigned
 * by then, or the parse failed; either way nothing reads them again. */
void markdown_core_names_release(markdown_core_names *state) {
    markdown_core_tree_free(&state->entries);
    state->count = state->parsed = 0;
    state->spliced = false;
    stems_free(state->edit);
    state->edit = NULL;
    markdown_core_key_index_free(&state->taken);
    markdown_core_free(state->renamed);
    state->renamed = NULL;
    state->renamed_count = state->renamed_capacity = 0;
    state->records_at = state->records_read = state->named = 0;
}

/* THE COMMITTED NAMES. An entry's anchor is the first name of its sequence
 * -- its base, then the base with -1, -2 and so on -- that no explicit anchor
 * reserves and no entry before it was given (anchors.md). The names taken
 * are one index. A name's entry holds who takes it, in `extra`: zero for an
 * explicit anchor, an entry's place among the entries plus one otherwise;
 * and in `value` the suffix an entry projecting to the name tries next. An
 * anchor's entry keys the label of the roster record that reserves it; a
 * entry's is read from the entry, its node's base and its suffix.
 *
 * An entry takes the first name of its sequence, from where it starts, that
 * is vacant or that a later entry holds, which then goes on from the
 * suffix of that name (it is displaced). Taking a name moves an entry on
 * along its sequence and frees no name, so the names everything before a
 * entry takes only grow, and the result is the one a pass over the
 * reservations and then the entries in order gives. An entry after every
 * other starts from its base and then the base's next suffix, which it
 * advances, and displaces no one; a displaced entry starts past its own
 * name, and does not read the suffix, below which later entries can hold
 * names it may take.
 *
 * A streaming parse keeps the index across snapshots, with where its reading
 * of the roster's ANCHOR records stands and how many entries it named.
 * Each snapshot first reads the records committed since the last and names
 * the entries commits parsed since, then names its tail in an overlay (A
 * SNAPSHOT'S NAMES). The finalize, and a parse that takes no snapshot, start
 * the index empty and read every record, and then every entry, which
 * displaces no one. Until some entry asks for a name, no record is read. */
typedef struct {
    markdown_core_parser *parser;
    markdown_core_names *state;
    /* The index the pass writes, the committed names or a snapshot's
     * overlay; and the committed names beneath an overlay, or NULL. */
    markdown_core_key_index *index;
    const markdown_core_key_index *beneath;
    /* Whether the pass starts empty, which reads only the declarations that
     * stand: the names they reserve are what reading every record would
     * leave, before any entry takes one. */
    bool fresh;
    /* What an entry's text projects to, which every entry of the pass uses
     * in turn. */
    markdown_core_strbuf name;
} name_pass;

/* The suffix of the name an entry holds in an index: its committed one,
 * but in a snapshot's overlay the one the snapshot gave a committed entry
 * it renamed (A COMMITTED NAME UNDER ANOTHER). */
static inline uint32_t holder_suffix(const markdown_core_names *state, const markdown_core_named *entry, bool overlay) {
    return overlay && entry->renamed ? name_of(state, entry)->suffix : entry->suffix;
}

/* What the index of the names taken compares: the name looked for, and
 * whether the slots are the overlay's. */
typedef struct {
    markdown_core_names *state;
    const name_view *name;
    bool overlay;
} name_probe;

/* A name and a holder's of one size whose bases are of one size too are
 * one name when their bases are and their suffixes are: their digits are
 * of one size, which only the suffix zero writes none of. */
static bool name_holds(const void *probe, const markdown_core_key_index_slot *slot) {
    const name_probe *look = probe;
    const name_view *name = look->name;
    if (!slot->extra) {
        return markdown_core_pieces_are(&name->pieces, slot->key, slot->key_len);
    }
    const markdown_core_named *holder = named_at(look->state, slot->extra - 1);
    const markdown_core_name *value = name_of(look->state, holder);
    const markdown_core_strbuf *base = &value->base;
    const uint32_t suffix = holder_suffix(look->state, holder, look->overlay);
    if (base->size == name->size) {
        if (suffix != name->suffix) {
            return false;
        }
        const unsigned char *front, *back;
        bufsize_t front_size, back_size;
        markdown_core_strbuf_span(base, 0, base->size, &front, &front_size, &back, &back_size);
        if (name->parts == 1 && !back_size) {
            return name->pieces.data[0] == front || !memcmp(name->pieces.data[0], front, (size_t)front_size);
        }
    }
    markdown_core_pieces held;
    value_base(value, &held);
    unsigned char tail[SUFFIX_ROOM];
    if (suffix) {
        markdown_core_pieces_add(&held, tail, suffix_write(tail, suffix));
    }
    return markdown_core_pieces_same(&name->pieces, &held);
}

/* The occupied entry for the name in the index the pass writes, or NULL. */
static markdown_core_key_index_slot *name_seek(name_pass *pass, const name_view *name) {
    const name_probe probe = {pass->state, name, pass->beneath != NULL};
    assert(name->hashed);
    return markdown_core_key_index_seek(pass->index, name->hash, name->pieces.size, name_holds, &probe);
}

/* The entry for the name among the names taken, occupied or vacant, in the
 * index the pass writes: an overlay first takes the committed entry of a
 * name it has none for. NULL when the index could not grow. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) markdown_core_key_index_slot *name_entry(name_pass *pass,
                                                                                                const name_view *name) {
    pass->state->work += (size_t)name->pieces.size + 1;
    assert(name->hashed);
    const uint64_t hash = name->hash;
    name_probe probe = {pass->state, name, pass->beneath != NULL};
    markdown_core_key_index_slot *entry =
        markdown_core_key_index_place(pass->index, hash, name->pieces.size, name_holds, &probe);
    if (entry && !entry->key && pass->beneath) {
        probe.overlay = false;
        const markdown_core_key_index_slot *committed =
            markdown_core_key_index_seek(pass->beneath, hash, name->pieces.size, name_holds, &probe);
        if (committed) {
            markdown_core_key_index_commit(pass->index, entry, committed->key);
            entry->value = committed->value;
            entry->extra = committed->extra;
        }
    }
    return entry;
}

/* A COMMITTED NAME UNDER ANOTHER. A snapshot that displaces a
 * committed entry gives its node the suffix the overlay gives it, and its
 * entry keeps the committed one. The snapshot lists the entry; the next
 * one gives the node its committed suffix back unless it displaces the
 * entry too. */
static bool rename_committed(markdown_core_parser *parser, markdown_core_names *state, size_t at) {
    markdown_core_named *entry = named_at(state, at);
    if (entry->renamed) {
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
    entry->renamed = true;
    return true;
}

/* Entry `at` takes the name of its base and `suffix`: its node shows it,
 * and its entry keeps it unless an overlay names a committed entry (A
 * COMMITTED HEADING UNDER ANOTHER NAME). False with the parse failed. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool name_take(name_pass *pass, size_t at, uint32_t suffix) {
    markdown_core_names *state = pass->state;
    markdown_core_named *entry = named_at(state, at);
    name_show(name_of(state, entry), suffix);
    if (pass->beneath && at < state->named) {
        return rename_committed(pass->parser, state, at);
    }
    entry->suffix = suffix;
    return true;
}

/* The entry `entry`, vacant or held by a later entry, names entry
 * `at`'s name. That entry, as its place plus one, or zero. */
static inline size_t claim_name(name_pass *pass, size_t at, markdown_core_key_index_slot *entry) {
    const size_t holder = entry->key ? entry->extra : 0;
    if (entry->key) {
        entry->key = HELD;
    } else {
        markdown_core_key_index_commit(pass->index, entry, HELD);
        entry->value = 1;
    }
    entry->extra = (uint32_t)(at + 1);
    return holder;
}

/* Entry `at`, after every entry the pass named, takes the first name of
 * its sequence that is vacant: `name`, the name of its base,
 * then the base with the suffix the base's entry keeps, which it advances,
 * so a run of entries projecting to one name tries each suffix once. Such a
 * entry is not committed under an overlay. */
static void take_last(name_pass *pass, size_t at, name_view *name) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_key_index *index = pass->index;
    markdown_core_key_index_slot *projected = name_entry(pass, name);
    markdown_core_key_index_slot *candidate = projected;
    size_t capacity = index->capacity;
    size_t suffix = 0;
    while (candidate && candidate->key) {
        /* A lookup that adds an entry can move the base's: a vacant
         * candidate, which ends the search, and in an overlay a name it
         * takes from the committed index. */
        if (index->capacity != capacity) {
            name_suffix(name, 0);
            projected = name_seek(pass, name);
            capacity = index->capacity;
        }
        suffix = projected->value++;
        name_suffix(name, suffix);
        candidate = name_entry(pass, name);
    }
    if (!candidate) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    assert(!(pass->beneath && at < pass->state->named));
    if (name_take(pass, at, (uint32_t)suffix)) {
        const size_t holder = claim_name(pass, at, candidate);
        assert(!holder);
        (void)holder;
    }
}

/* Entry `at`, displaced from the name it holds, takes the first name past
 * it in its sequence that is vacant or that a later entry holds, and so
 * on for each entry displaced in turn, each later than the last. */
static void displace(name_pass *pass, size_t at) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    for (size_t holder = at + 1; holder && !parser->error;) {
        at = holder - 1;
        const markdown_core_named *entry = named_at(state, at);
        size_t suffix = holder_suffix(state, entry, pass->beneath != NULL);
        name_view name;
        name_value(&name, name_of(state, entry));
        markdown_core_key_index_slot *candidate;
        do {
            name_suffix(&name, ++suffix);
            candidate = name_entry(pass, &name);
            if (!candidate) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return;
            }
        } while (candidate->key && (!candidate->extra || candidate->extra <= at + 1));
        holder = name_take(pass, at, (uint32_t)suffix) ? claim_name(pass, at, candidate) : 0;
    }
}

/* The record's name reserved: a vacant name becomes the record's, and one a
 * entry holds is taken from it, which goes on along its sequence. A
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
    name_view name;
    markdown_core_pieces_clear(&name.pieces);
    markdown_core_pieces_add(&name.pieces, label.data, label.len);
    name_hashed(&name);
    if (record->withdraws) {
        assert(!pass->beneath);
        if (!record->live) {
            pass->state->work += (size_t)label.len + 1;
            markdown_core_key_index_remove(index, name_seek(pass, &name));
        }
        return;
    }
    markdown_core_key_index_slot *entry = name_entry(pass, &name);
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

/* The projection of the children of the entry's node from `from` up to
 * `to`, appended to `base` (names.h, the owner's `project`): false, with
 * the parse failed, when storage could not be had. */
static bool named_project(markdown_core_parser *parser, markdown_core_names *state, const markdown_core_named *entry,
                          uint32_t from, uint32_t to, markdown_core_strbuf *base) {
    const markdown_core_element_instance *owner = state->owners[entry->owner];
    if (!owner->element->names->project(owner, entry->node, from, to, base, &state->work) || base->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    return true;
}

/* The entry's node keeps the base its text projects to, unless it keeps
 * it already. */
static void base_read(name_pass *pass, markdown_core_named *entry) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_strbuf *name = &pass->name;
    markdown_core_strbuf_clear(name);
    if (parser->error || !named_project(parser, pass->state, entry, 0, entry->node->child_count, name)) {
        return;
    }
    /* An entry whose text projects to nothing borrows the base "section";
     * any other copies the projection, so the pass keeps its storage for the
     * next entry's. */
    markdown_core_name *value = name_of(pass->state, entry);
    const markdown_core_strbuf *read = name->size ? name : &SECTION;
    markdown_core_pieces base;
    value_base(value, &base);
    if (!markdown_core_pieces_are(&base, read->ptr, read->size)) {
        if (!name->size) {
            if (!markdown_core_strbuf_borrows(&value->base)) {
                markdown_core_strbuf_free(&value->base);
            }
            value->base = SECTION;
        } else {
            if (markdown_core_strbuf_borrows(&value->base) || value->base.split) {
                if (!markdown_core_strbuf_borrows(&value->base)) {
                    markdown_core_strbuf_free(&value->base);
                }
                markdown_core_strbuf_init(&value->base, 0);
            }
            markdown_core_strbuf_set(&value->base, name->ptr, name->size);
            if (value->base.oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return;
            }
        }
        name_unshow(value);
    }
    entry->named = true;
}

/* The entries from `first` to `end` that do not author their anchors
 * named, each after every entry the pass named before it. An entry whose
 * node keeps its base reads it there. */
static void name_headings(name_pass *pass, size_t first, size_t end) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    for (size_t at = first; at < end && !parser->error; at++) {
        markdown_core_named *entry = named_at(state, at);
        if (entry->authored) {
            continue;
        }
        if (entry->named) {
            state->work += (size_t)name_of(state, entry)->base.size;
        } else {
            base_read(pass, entry);
        }
        if (!parser->error) {
            name_view name;
            name_value(&name, name_of(state, entry));
            take_last(pass, at, &name);
        }
    }
}

/* A SNAPSHOT'S NAMES. The snapshot's tail -- the ANCHOR records its
 * completion made, then the entries it parsed, the pending block's copy
 * among them -- takes its names in an overlay over the committed names, which
 * stay as the commits left them. A reservation can displace committed
 * entries, which show the names the overlay gives them (A COMMITTED HEADING
 * UNDER ANOTHER NAME); then each committed entry the last snapshot renamed
 * and this one did not shows its committed name again, and the ones this one
 * renamed are the list. */
static void name_snapshot(name_pass *pass, markdown_core_roster_cursor cursor) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    const size_t total = names_total(state);
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
        markdown_core_named *entry = named_at(state, state->renamed[i]);
        if (!entry->renamed) {
            name_show(name_of(state, entry), entry->suffix);
        }
    }
    size_t count = 0;
    for (size_t i = earlier; i < state->renamed_count; i++) {
        named_at(state, state->renamed[i])->renamed = false;
        state->renamed[count++] = state->renamed[i];
    }
    state->renamed_count = count;
    markdown_core_key_index_free(&overlay);
    pass->index = &state->taken;
    pass->beneath = NULL;
}

/* The committed names let go, for a pass that starts empty, which names
 * every entry again. */
static void names_reset(markdown_core_names *state) {
    state->renamed_count = 0;
    markdown_core_key_index_free(&state->taken);
    state->records_at = state->records_read = state->named = 0;
}

/* Whether an entry that starts before `place` holds the name of the stem
 * `id` and `suffix`. Only two can (THE NAMES AN EDIT MOVES): the first of
 * the name's own stem, when it holds its base, and one of the parent's
 * that holds the name's suffix. The suffixes of a stem's entries rise in
 * order, from its base at most at the first: each takes its base, which
 * stays taken, or the next of its stem's suffixes that is free. */
static bool name_held(markdown_core_names *state, uint32_t id, uint32_t suffix, size_t place) {
    markdown_core_name_edit *names = state->edit;
    size_t at;
    const uint32_t own = name_stem(state, id, suffix);
    if (own && stem_present(stem_of(names, own)) && stem_place_of(state, stem_of(names, own), 0) < place &&
        !stem_member(state, stem_of(names, own), 0, &at)->suffix) {
        return true;
    }
    uint32_t up = id, held = suffix;
    if (!suffix) {
        up = stem_parent(state, id, &held);
    }
    if (!up || !stem_present(stem_of(names, up))) {
        return false;
    }
    const markdown_core_name_stem *stem = stem_of(names, up);
    size_t low = 0, high = stem_before(state, stem, place);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const uint32_t own_suffix = stem_member(state, stem, middle, &at)->suffix;
        if (own_suffix == held) {
            return true;
        }
        if (own_suffix < held) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

/* What a pass of the names moved (THE NAMES AN EDIT MOVES): for each stem
 * and suffix, how many more of its entries take the name than took it,
 * over the entries the pass named and those the edit let go, each count
 * found by its index; `nonzero` counts the names whose count is not zero. */
typedef struct {
    uint32_t stem, suffix;
    size_t count;
} name_count;

typedef struct {
    markdown_core_key_index index;
    name_count *counts;
    size_t count, room;
    size_t nonzero;
} name_moves;

typedef struct {
    const name_moves *moves;
    uint32_t stem, suffix;
} move_probe;

static bool move_holds(const void *probe, const markdown_core_key_index_slot *slot) {
    const move_probe *look = probe;
    const name_count *count = &look->moves->counts[slot->value];
    return count->stem == look->stem && count->suffix == look->suffix;
}

/* The count of the stem's name of `suffix` moves by `step`. False when
 * storage could not be had. */
static bool name_move(name_moves *moves, uint32_t stem, uint32_t suffix, int step) {
    const move_probe probe = {moves, stem, suffix};
    markdown_core_key_index_slot *slot = markdown_core_key_index_place(
        &moves->index, markdown_core_key_index_hash((markdown_core_measure){stem, suffix}),
        (bufsize_t)sizeof(probe.stem) + (bufsize_t)sizeof(probe.suffix), move_holds, &probe);
    if (!slot) {
        return false;
    }
    if (!slot->key) {
        name_count *counts = markdown_core_reserve(moves->counts, &moves->room, moves->count + 1, sizeof(*counts));
        if (!counts) {
            return false;
        }
        moves->counts = counts;
        counts[moves->count] = (name_count){stem, suffix, 0};
        markdown_core_key_index_commit(&moves->index, slot, HELD);
        slot->value = moves->count++;
    }
    /* The count wraps as a size does, so that only zero is read: a count
     * below zero is a size past half of any. */
    name_count *count = &moves->counts[slot->value];
    const size_t was = count->count, now = was + (size_t)(ptrdiff_t)step;
    count->count = now;
    if (!was) {
        moves->nonzero++;
    } else if (!now) {
        moves->nonzero--;
    }
    return true;
}

static void name_moves_free(name_moves *moves) {
    markdown_core_free(moves->counts);
    markdown_core_key_index_free(&moves->index);
}

/* A stem's next entry the pass names: where it starts, the stem, and
 * its place among the stem's. */
typedef struct {
    size_t place;
    uint32_t stem;
    size_t next;
} stem_cursor;

/* The heap of cursors, least place first, its `count` from `at` sifted. */
static void cursors_down(stem_cursor *heap, size_t count, size_t at) {
    for (;;) {
        size_t least = at;
        const size_t left = 2 * at + 1, other = left + 1;
        if (left < count && heap[left].place < heap[least].place) {
            least = left;
        }
        if (other < count && heap[other].place < heap[least].place) {
            least = other;
        }
        if (least == at) {
            return;
        }
        const stem_cursor swap = heap[at];
        heap[at] = heap[least];
        heap[least] = swap;
        at = least;
    }
}

/* A pass over the groups of the stems an edit touched (kin_rename):
 * the heap of its stems' cursors, least place first, and the mark of the
 * stems it holds. */
typedef struct {
    markdown_core_names *state;
    stem_cursor *heap;
    size_t count, room;
    uint32_t mark;
} stem_heap;

/* The cursor at the heap's `at` sifted up. */
static void cursors_up(stem_cursor *heap, size_t at) {
    while (at) {
        const size_t parent = (at - 1) / 2;
        if (heap[parent].place <= heap[at].place) {
            return;
        }
        const stem_cursor swap = heap[at];
        heap[at] = heap[parent];
        heap[parent] = swap;
        at = parent;
    }
}

/* The stem `id` joins the pass from its first entry at or past `place`,
 * trying the suffix past its last entry before that took one, unless the
 * pass holds it or it holds no entry. False when storage could not be
 * had. */
static bool stem_join(stem_heap *pass, uint32_t id, size_t place) {
    markdown_core_names *state = pass->state;
    markdown_core_name_stem *stem = stem_of(state->edit, id);
    if (!stem->measure.size || stem->seen == pass->mark) {
        return true;
    }
    stem->seen = pass->mark;
    const size_t first = stem_before(state, stem, place);
    if (first == stem->members.count) {
        return true;
    }
    stem->suffix = 1;
    for (size_t k = first; k > 0; k--) {
        size_t at;
        const markdown_core_named *entry = stem_member(state, stem, k - 1, &at);
        if (entry->suffix) {
            stem->suffix = (size_t)entry->suffix + 1;
            break;
        }
    }
    stem_cursor *wider = markdown_core_reserve(pass->heap, &pass->room, pass->count + 1, sizeof(*wider));
    if (!wider) {
        return false;
    }
    pass->heap = wider;
    pass->heap[pass->count] = (stem_cursor){stem_place_of(state, stem, first), id, first};
    cursors_up(pass->heap, pass->count++);
    return true;
}

/* The stem the name of the stem `id` and `suffix` is the base of joins the
 * pass from `place`, as the name was taken or let go there. */
static inline bool name_join(stem_heap *pass, uint32_t id, uint32_t suffix, size_t place) {
    const uint32_t own = name_stem(pass->state, id, suffix);
    return !own || stem_join(pass, own, place);
}

/* The parent of the stem `id` joins the pass from `place`. */
static inline bool parent_join(stem_heap *pass, uint32_t id, size_t place) {
    uint32_t suffix;
    const uint32_t up = stem_parent(pass->state, id, &suffix);
    return !up || stem_join(pass, up, place);
}

/* The groups of the stems an edit touched named again (THE NAMES AN EDIT
 * MOVES): their entries in order, each taking the first name of its
 * sequence that no anchor standing reserves and no entry before it
 * holds, past the suffixes its base's earlier entries tried. The pass
 * starts at the first entry of the stems the reading touched and of
 * their parents, where the rest point is, as those before it keep their
 * names, unless an anchor the edit made or let go names a stem of them,
 * which moves the names of any: then it starts at their first. A stem
 * joins it where an entry of the pass takes or lets go a name the stem is
 * the base of, or the parent's where one of its entries takes or lets go
 * its base: only a name binds two stems, so a group joins as its names
 * move, and an edit beside one of many stems of one parent names few of
 * them. It ends once every entry the reading made is named and the
 * entries named, with those the edit let go, took the names they took
 * before, stem by stem: the old entries after them then take theirs
 * again. */
static void kin_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    markdown_core_name_edit *names = state->edit;
    stem_heap heap = {.state = state, .mark = ++names->pass};
    bool whole = false;
    for (size_t i = 0; i < names->touched_count; i++) {
        whole = whole || stem_of(names, names->touched[i])->anchored == heap.mark;
    }
    const size_t from = whole ? 0 : names->rest;
    name_moves moves = {.count = 0};
    bool ok = markdown_core_key_index_init(&moves.index, 8);
    for (size_t i = 0; ok && i < names->touched_count; i++) {
        ok = stem_join(&heap, names->touched[i], from) && parent_join(&heap, names->touched[i], from);
    }
    /* The names the edit let go, of the stems of the pass, and the stems
     * they are the bases of. */
    for (size_t i = 0; ok && i < names->lost_count; i++) {
        const markdown_core_name_lost lost = names->lost[i];
        if (stem_of(names, lost.stem)->seen == heap.mark) {
            ok = name_move(&moves, lost.stem, lost.suffix, -1) && name_join(&heap, lost.stem, lost.suffix, from);
        }
    }
    while (ok && heap.count && !parser->error) {
        const stem_cursor cursor = heap.heap[0];
        markdown_core_name_stem *stem = stem_of(names, cursor.stem);
        size_t at;
        markdown_core_named *entry = stem_member(state, stem, cursor.next, &at);
        const bool made = made_at(state, at);
        if (!whole && !made && !moves.nonzero) {
            break;
        }
        markdown_core_pieces base;
        stem_text(state, cursor.stem, &base);
        uint32_t suffix = 0;
        name_view name;
        name_base(&name, &base, stem->measure);
        for (;;) {
            name_suffix(&name, suffix);
            bool failed = false;
            const bool reserved =
                markdown_core_roster_find_name(&parser->roster, &name.pieces, name.measure, &failed) != NULL;
            if (failed) {
                ok = false;
                break;
            }
            if (!reserved && !name_held(state, cursor.stem, suffix, cursor.place)) {
                break;
            }
            suffix = (uint32_t)stem->suffix++;
        }
        if (!ok) {
            break;
        }
        const uint32_t old = entry->suffix;
        if (made || old != suffix) {
            /* The stems the old name and the new are the bases of join, and
             * the parent when the entry takes or lets go its base. */
            const bool based = made || !old;
            ok = (made || (name_move(&moves, cursor.stem, old, -1) &&
                           (!old || name_join(&heap, cursor.stem, old, cursor.place)))) &&
                 name_move(&moves, cursor.stem, suffix, 1) &&
                 (!suffix || name_join(&heap, cursor.stem, suffix, cursor.place)) &&
                 (based == !suffix || parent_join(&heap, cursor.stem, cursor.place));
        }
        if (!ok) {
            break;
        }
        entry->suffix = suffix;
        name_show(name_of(state, entry), suffix);
        /* A stem that joined starts past the entry, which stays on top. */
        assert(heap.heap[0].stem == cursor.stem && heap.heap[0].next == cursor.next);
        if (cursor.next + 1 < stem->members.count) {
            heap.heap[0].place = stem_place_of(state, stem, cursor.next + 1);
            heap.heap[0].next = cursor.next + 1;
        } else {
            heap.heap[0] = heap.heap[--heap.count];
        }
        cursors_down(heap.heap, heap.count, 0);
    }
    if (!ok && !parser->error) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    name_moves_free(&moves);
    markdown_core_free(heap.heap);
}

/* THE NEW BASES. The base of an entry the edit's reading made waits for
 * the edit's end, while its node keeps the base it had, which a stem may
 * read as its witness: an entry the reading read whole projects its text
 * whole, and one whose reading leapt in it (blocks.c, THE SEAMS) keeps its
 * old base but for the projection of the children its parse made, which
 * takes the place of the old bytes between those of the children it kept,
 * as the measures its seams keep say how many (markdown_core_parser_leapt).
 * Its name is read and measured from those parts, so an edit in a long
 * entry projects only what it changed, and compares only that with the
 * old base (pieces.h). The edit's end gives each node its new base: in
 * place, where only the bytes it changed move (buffer.h,
 * markdown_core_strbuf_split_replace). */
static void base_new(name_pass *pass, const markdown_core_named *entry, markdown_core_name_base *base) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    markdown_core_node *node = entry->node;
    *base = (markdown_core_name_base){.mid = MARKDOWN_CORE_BUF_INIT()};
    const markdown_core_strbuf *old = old_base(state, entry);
    markdown_core_leapt leapt = {MARKDOWN_CORE_MEASURE_NONE, MARKDOWN_CORE_MEASURE_NONE, 0, 0};
    uint32_t from = 0, to = node->child_count;
    if (old->size && markdown_core_parser_leapt(parser, node, &leapt) &&
        (size_t)leapt.front.size + (size_t)leapt.back.size <= (size_t)old->size) {
        *base = (markdown_core_name_base){old, (bufsize_t)leapt.front.size, (bufsize_t)leapt.back.size,
                                          MARKDOWN_CORE_BUF_INIT(), MARKDOWN_CORE_MEASURE_NONE};
        from = leapt.from;
        to = leapt.to;
    }
    if (!named_project(parser, state, entry, from, to, &base->mid)) {
        return;
    }
    base->measure = markdown_core_measure_of(base->mid.ptr, (size_t)base->mid.size);
    if (base->old) {
        base->measure = markdown_core_measure_join(markdown_core_measure_join(leapt.front, base->measure), leapt.back);
    }
    if (!base->measure.size) {
        base->old = NULL;
        markdown_core_strbuf_puts(&base->mid, "section");
        base->measure = markdown_core_measure_of(base->mid.ptr, (size_t)base->mid.size);
    }
    if (base->mid.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}

/* Each node the edit's reading made an entry of takes its new base (THE
 * NEW BASES), and one whose attributes write its anchor keeps none. False
 * when storage could not be had. */
static bool bases_commit(markdown_core_names *state) {
    markdown_core_name_edit *names = state->edit;
    bool ok = true;
    for (size_t i = names->fresh; i < state->count; i++) {
        markdown_core_named *entry = named_at(state, i);
        markdown_core_name_base *base = &names->bases[i - names->fresh];
        markdown_core_name *value = name_of(state, entry);
        if (entry->authored) {
            base_drop(value);
            continue;
        }
        markdown_core_strbuf *old = (markdown_core_strbuf *)base->old;
        if (old && !markdown_core_strbuf_borrows(old)) {
            ok = ok && markdown_core_strbuf_split_replace(old, base->front, old->size - base->back, base->mid.ptr,
                                                          base->mid.size);
            if (old != &value->base) {
                /* A base kept aside (markdown_core_names_retire) is the node's again. */
                markdown_core_strbuf_free(&value->base);
                value->base = *old;
                markdown_core_strbuf_init(old, 0);
            }
        } else if (old) {
            /* A base the node borrows (node.c, THE VIEW) is read into one of
             * its own. */
            markdown_core_pieces pieces;
            base_pieces(base, &pieces);
            markdown_core_strbuf own = MARKDOWN_CORE_BUF_INIT();
            for (uint32_t k = 0; k < pieces.count; k++) {
                markdown_core_strbuf_put(&own, pieces.data[k], pieces.sizes[k]);
            }
            ok = ok && !own.oom;
            markdown_core_strbuf_swap(&own, &base->mid);
            markdown_core_strbuf_free(&own);
            old = NULL;
        }
        if (!old) {
            markdown_core_pieces now;
            value_base(value, &now);
            if (!markdown_core_pieces_are(&now, base->mid.ptr, base->mid.size)) {
                if (markdown_core_strbuf_borrows(&value->base)) {
                    markdown_core_strbuf_init(&value->base, 0);
                }
                markdown_core_strbuf_swap(&value->base, &base->mid);
            }
        }
        name_unshow(value);
        entry->named = true;
    }
    return ok;
}

/* An edit's entries named (THE NAMES AN EDIT MOVES): each the reading
 * made takes its new base (THE NEW BASES) and joins its stem, the stems an
 * anchor it made or let go names are touched with their parents, and the
 * groups of the stems touched are named again; then the stems with neither
 * entry nor child go, and the nodes take their new bases. */
static void edit_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_names *state = pass->state;
    markdown_core_name_edit *names = state->edit;
    const size_t made = state->count - names->fresh;
    markdown_core_name_base *bases =
        made ? markdown_core_reserve(names->bases, &names->base_room, made, sizeof(*bases)) : names->bases;
    if (made && !bases) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else {
        names->bases = bases;
    }
    for (size_t i = names->fresh; i < state->count && !parser->error; i++) {
        markdown_core_named *entry = named_at(state, i);
        markdown_core_name_base *base = &names->bases[names->base_count++];
        *base = (markdown_core_name_base){.mid = MARKDOWN_CORE_BUF_INIT()};
        entry->stem = 0;
        if (!entry->authored) {
            base_new(pass, entry, base);
        }
    }
    for (size_t i = names->fresh; i < state->count && !parser->error; i++) {
        markdown_core_named *entry = named_at(state, i);
        if (entry->authored) {
            continue;
        }
        const markdown_core_name_base *base = &names->bases[i - names->fresh];
        markdown_core_pieces text;
        base_pieces(base, &text);
        const uint32_t id = stem_make(state, &text, base->measure);
        if (!id || !stem_add(state, id, entry->id)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        entry->stem = id;
    }
    markdown_core_strbuf anchors = MARKDOWN_CORE_BUF_INIT();
    if (!parser->error && !markdown_core_roster_anchors(&parser->roster, names->rest, &anchors)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    for (bufsize_t at = 0; !parser->error && at < anchors.size;) {
        const bufsize_t size = (bufsize_t)strlen((const char *)anchors.ptr + at);
        markdown_core_pieces label;
        markdown_core_pieces_clear(&label);
        markdown_core_pieces_add(&label, anchors.ptr + at, size);
        const markdown_core_measure measure = markdown_core_measure_of(anchors.ptr + at, (size_t)size);
        markdown_core_measure parent;
        uint32_t suffix;
        const uint32_t own = stem_find(state, &label, measure);
        uint32_t up = 0;
        if (name_parent(&label, measure, &parent, &suffix)) {
            markdown_core_pieces head;
            markdown_core_pieces_head(&label, (bufsize_t)parent.size, &head);
            up = stem_find(state, &head, parent);
        }
        if (own) {
            stem_of(names, own)->anchored = names->pass + 1;
        }
        if (up) {
            stem_of(names, up)->anchored = names->pass + 1;
        }
        if ((own && !stem_touch(names, own)) || (up && !stem_touch(names, up))) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        at += size + 1;
    }
    markdown_core_strbuf_free(&anchors);
    if (!parser->error) {
        kin_rename(pass);
    }
    for (size_t i = 0; i < names->touched_count; i++) {
        stem_release(names, names->touched[i]);
    }
    names->touched_count = 0;
    if (!parser->error && !bases_commit(state)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    bases_free(names);
    names->lost_count = 0;
    names->fresh = state->count;
}

/* Each entry's anchor (THE COMMITTED NAMES): an authored one stays, and a
 * computed one is the entry's base and the suffix chosen; only an entry
 * whose name changes is written. A snapshot moves the committed names on,
 * by the records and the entries its commit added before the oldest
 * candidate's mark, and names the rest over them, its tail among them; the
 * finalize names everything from an empty index. */
void markdown_core_names_finish(markdown_core_parser *parser) {
    markdown_core_names *state = &parser->names;
    /* No node asked for a name: there is nothing to name. */
    if (!state->entries.count && !state->edit) {
        return;
    }
    const bool snapshot = parser->snapshot;
    names_follow(state);
    if (state->edit && !snapshot) {
        name_pass pass = {.parser = parser, .state = state, .name = MARKDOWN_CORE_BUF_INIT()};
        if (!back_drop(state)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        } else {
            edit_rename(&pass);
        }
        markdown_core_strbuf_free(&pass.name);
        return;
    }
    const size_t total = names_total(state);
    size_t committed = total, committed_records = parser->roster.count;
    if (snapshot) {
        committed = markdown_core_parser_kept_names(parser, &committed_records);
    } else {
        names_reset(state);
    }
    if (!state->taken.size) {
        /* Nothing is named yet: the authored entries ask for no name. */
        while (state->named < committed && named_at(state, state->named)->authored) {
            state->named++;
        }
        size_t first = state->named;
        while (first < total && named_at(state, first)->authored) {
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
    read_records(&pass, &cursor, snapshot ? committed_records : roster->count);
    state->records_at = cursor.at;
    state->records_read = records - cursor.left;
    name_headings(&pass, state->named, committed);
    state->named = committed;
    if (snapshot && !parser->error) {
        name_snapshot(&pass, cursor);
    }
    markdown_core_strbuf_free(&pass.name);
}
typedef struct {
    bufsize_t size;
    unsigned char text[];
} shown_anchor;

bool markdown_core_name_anchor(markdown_core_name *value, markdown_core_chunk *shows) {
    const markdown_core_strbuf *base = &value->base;
    if (!base->size) {
        *shows = (markdown_core_chunk){0};
        return true;
    }
    if (!value->suffix && !base->split) {
        *shows = (markdown_core_chunk){base->ptr, base->size, 0};
        return true;
    }
    const shown_anchor *shown = markdown_core_claim_read(&value->shown);
    if (!shown) {
        markdown_core_pieces pieces;
        value_base(value, &pieces);
        name_view name;
        name_make(&name, &pieces, MARKDOWN_CORE_MEASURE_NONE, value->suffix);
        shown_anchor *made = markdown_core_alloc(1, sizeof(*made) + (size_t)name.pieces.size + 1);
        if (!made) {
            return false;
        }
        markdown_core_pieces_copy(&name.pieces, made->text);
        made->text[name.pieces.size] = '\0';
        made->size = name.pieces.size;
        shown = markdown_core_claim(&value->shown, made);
        if (shown != made) {
            markdown_core_free(made);
        }
    }
    *shows = (markdown_core_chunk){(unsigned char *)shown->text, shown->size, 0};
    return true;
}

/* THE OWNER'S SIDE (names.h). */
void markdown_core_names_init(markdown_core_names *names, const markdown_core_element_instance *const *owners) {
    names->owners = owners;
}

/* (names.h) The node goes into the entries, in its place, numbered, in an
 * edit's parse; the names are assigned as the parse ends. */
void markdown_core_parser_name(markdown_core_parser *parser, const markdown_core_element_instance *self,
                               markdown_core_node *node) {
    markdown_core_names *state = &parser->names;
    names_follow(state);
    uint8_t owner = 0;
    while (state->owners[owner] != self) {
        assert(state->owners[owner]);
        owner++;
    }
    markdown_core_tree *entries = entries_of(state);
    markdown_core_named entry = {.node = node, .start = node->place.start, .owner = owner};
    if (!state->edit) {
        markdown_core_named *pushed = named_tree_push(entries);
        if (pushed) {
            *pushed = entry;
            state->count++;
        } else {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return;
    }
    entry.id = named_number(state->edit);
    if (!entry.id || !named_tree_insert(entries, entries->front, entry, true)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    state->count++;
}

bool markdown_core_names_order(markdown_core_parser *parser, size_t *first, size_t *count) {
    markdown_core_names *state = &parser->names;
    names_follow(state);
    *first = state->parsed;
    *count = state->count;
    if (!entries_order(state, parser, *first, *count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    return true;
}

markdown_core_named *markdown_core_names_span(markdown_core_parser *parser, size_t first, size_t count) {
    return named_span(&parser->names, first, count);
}

markdown_core_named *markdown_core_names_at(markdown_core_parser *parser, size_t at) {
    return named_at(&parser->names, at);
}

/* (names.h) The entry's fields after `start` are written anew. */
void markdown_core_names_read(markdown_core_named *entry, bool authored) {
    *entry = (markdown_core_named){
        .node = entry->node, .start = entry->start, .id = entry->id, .owner = entry->owner, .authored = authored};
}

void markdown_core_names_read_to(markdown_core_parser *parser, size_t parsed) { parser->names.parsed = parsed; }

void markdown_core_names_reread(markdown_core_parser *parser, markdown_core_node *node, bool authored) {
    markdown_core_names *state = &parser->names;
    markdown_core_named *entry = parsed_named(state, node->place.start);
    if (authored) {
        base_drop(name_of(state, entry));
    }
    entry->authored = authored;
    entry->named = false;
}

bool markdown_core_names_retire(markdown_core_parser *parser, const markdown_core_element_instance *self,
                                markdown_core_node *node) {
    markdown_core_name_edit *names = parser->names.edit;
    markdown_core_name *value = value_of(self->element, node);
    if (!names || !markdown_core_strbuf_owns(&value->base)) {
        return true;
    }
    markdown_core_name_retired *retired =
        markdown_core_reserve(names->retired, &names->retired_room, names->retired_count + 1, sizeof(*retired));
    markdown_core_strbuf *base = retired ? markdown_core_alloc(1, sizeof(*base)) : NULL;
    if (retired) {
        names->retired = retired;
    }
    if (!base) {
        return false;
    }
    *base = value->base;
    markdown_core_strbuf_init(&value->base, 0);
    names->retired[names->retired_count++] = (markdown_core_name_retired){node, base};
    /* A stem whose last entry to go was this one reads the base where it
     * is kept now. */
    for (size_t i = 0; i < names->lost_count; i++) {
        markdown_core_name_stem *stem = &names->stems[names->lost[i].stem - 1];
        if (stem->relic == &value->base) {
            stem->relic = base;
        }
    }
    return true;
}
