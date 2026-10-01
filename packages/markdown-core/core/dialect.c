#include <assert.h>
#include <string.h>

#include "alloc.h"
#include "block_internal.h"
#include "dialect.h"

/* Whether a descriptor is refused. The rule: an element takes part in
 * the finish stage as a LOCAL step or as a GLOBAL pass, never both
 * (markdown-core-element-api.h states the invariant). A descriptor that
 * declares both would run its step from inside the walk and its pass after it,
 * and nothing in either hook's contract says what the second may assume about
 * the first's work; that is two concerns, which is two elements. A step is
 * asked once per event, so a kind in both of its lists -- one EXIT declared
 * twice -- is refused rather than delivered twice. A step asked at no kind
 * would never be called, and is refused rather than silently kept. And a kind
 * the dispatch table cannot index -- an ordinal at or past
 * MARKDOWN_CORE_NODE_KIND_COUNT, or a value of neither class -- would share
 * the table's one out-of-table key and be asked at every such node's events
 * instead of its own, so it is refused too: that key is never declared.
 *
 * The document lifecycle is one concern too. The engine calls every one of
 * its hooks but `observe_inline` without asking, on whichever element owns
 * it, so an element that declares part of it would have the engine call
 * through a NULL the moment it became the owner: it declares all of them or
 * none.
 *
 * A flanking-transparent byte is ASCII. Flanking tests a decoded scalar
 * against these bytes, and only a scalar below 0x80 is its own byte. Its walk
 * back also stops at one of them that a continuation byte follows, which
 * UTF-8 never puts after an ASCII byte; a continuation byte that were
 * transparent would let every walk run to the start of its paragraph again.
 *
 * A dialect holds an element once: sealing makes one instance of each element
 * and hands every hook its own (markdown-core-element-api.h, "AN ELEMENT AS
 * ONE PARSE HOLDS IT"), so a descriptor attached twice would be two instances
 * answering to one identity. That is a fact of the whole dialect, which
 * attachment checks against the elements already attached
 * (S_element_attached). */
static bool S_finish_kind_indexable(markdown_core_node_type kind) {
    unsigned class = (unsigned)kind & MARKDOWN_CORE_NODE_TYPE_MASK;
    return (class == MARKDOWN_CORE_NODE_TYPE_BLOCK || class == MARKDOWN_CORE_NODE_TYPE_INLINE) &&
           ((unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK) < MARKDOWN_CORE_NODE_KIND_COUNT;
}

static bool S_owns_document_lifecycle(const markdown_core_element *element) {
    return element->init_document && element->dispose_document && element->read_document_prefix &&
           element->prepare_document && element->finish_document && element->publish_document;
}

static bool S_element_refused(const markdown_core_element *element) {
    /* Both a finish step and a postprocess pass. */
    if (element->finish_step && element->postprocess_func) {
        return true;
    }
    /* Kinds to ask a finish step at, without a finish step. */
    if ((element->finish_exit_kinds || element->finish_scope_kinds) && !element->finish_step) {
        return true;
    }
    /* A finish step at a kind outside the kind table, or asked at no kind. */
    if (element->finish_step) {
        const markdown_core_node_type *lists[] = {element->finish_exit_kinds, element->finish_scope_kinds};
        bool asked = false;
        for (size_t list = 0; list < 2; list++) {
            for (const markdown_core_node_type *kind = lists[list]; kind && *kind; kind++) {
                if (!S_finish_kind_indexable(*kind)) {
                    return true;
                }
                asked = true;
            }
        }
        if (!asked) {
            return true;
        }
    }
    /* A kind as both a finish exit kind and a finish scope kind. */
    for (const markdown_core_node_type *exit = element->finish_exit_kinds; exit && *exit; exit++) {
        for (const markdown_core_node_type *scope = element->finish_scope_kinds; scope && *scope; scope++) {
            if (*exit == *scope) {
                return true;
            }
        }
    }
    /* Only part of the document lifecycle. */
    if ((element->init_document || element->dispose_document || element->read_document_prefix ||
         element->prepare_document || element->finish_document || element->publish_document ||
         element->observe_inline) &&
        !S_owns_document_lifecycle(element)) {
        return true;
    }
    /* A flanking-transparent byte outside ASCII. */
    for (const unsigned char *c = (const unsigned char *)element->flanking_transparent; c && *c; c++) {
        if (*c >= 0x80) {
            return true;
        }
    }
    return false;
}

/* Whether `element` is one of the first `count` elements. */
static bool S_element_attached(const markdown_core_element *const *elements, size_t count,
                               const markdown_core_element *element) {
    for (size_t i = 0; i < count; i++) {
        if (elements[i] == element) {
            return true;
        }
    }
    return false;
}

void markdown_core_dialect_builder_init(markdown_core_dialect_builder *builder,
                                        const markdown_core_element *const *elements, size_t count) {
#ifndef NDEBUG
    for (size_t i = 0; i < count; i++) {
        assert(!S_element_refused(elements[i]) && !S_element_attached(elements, i, elements[i]));
    }
#endif
    builder->elements = elements;
    builder->element_allocation = NULL;
    builder->element_capacity = 0;
    builder->element_count = count;
}

/* Setup extends the dialect the instance will be sealed with. The core
 * dialect is borrowed; the first attachment copies it into a list the builder
 * owns, which then grows geometrically. Allocation failure, a descriptor the
 * one registration rule refuses, and a full dialect leave the builder as it
 * was. */
int markdown_core_dialect_builder_attach(markdown_core_dialect_builder *builder, const markdown_core_element *element) {
    size_t count = builder->element_count;
    if (S_element_refused(element) || count >= MARKDOWN_CORE_ELEMENT_LIMIT ||
        S_element_attached(builder->elements, count, element)) {
        return 0;
    }
    const markdown_core_element **entries =
        markdown_core_reserve(builder->element_allocation, &builder->element_capacity, count + 1, sizeof(*entries));
    if (!entries) {
        return 0;
    }
    if (!builder->element_allocation && count) {
        memcpy(entries, builder->elements, count * sizeof(*entries));
    }
    entries[count] = element;
    builder->element_allocation = entries;
    builder->elements = entries;
    builder->element_count = count + 1;
    return 1;
}

const markdown_core_element *const *markdown_core_dialect_builder_elements(const markdown_core_dialect_builder *builder,
                                                                           size_t *count) {
    *count = builder->element_count;
    return builder->elements;
}

void markdown_core_dialect_builder_dispose(markdown_core_dialect_builder *builder) {
    markdown_core_free(builder->element_allocation);
    builder->element_allocation = NULL;
    builder->element_capacity = 0;
}

/* Whether `element` implements `hook`. The PROBE family carries a second
 * static condition: `markdown_core_parser_has_block_start` only ever asked a
 * probe that also interrupts a paragraph, and that is a descriptor property,
 * so it belongs in the projection rather than in the loop. */
static bool S_element_implements(const markdown_core_element *element, markdown_core_block_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return element->scan_block_start != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return element->try_interrupting_block != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return element->try_opening_block != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
        return element->try_opening_paragraph != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_PROBE:
        return element->probe_block != NULL && element->interrupts_paragraph;
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return false;
}

/* The gate `element` declares for `hook`, or an ungated one. A family gains a
 * gate by adding a descriptor field and a case here; the dispatcher never
 * learns an element's name. */
static markdown_core_block_gate S_element_gate(const markdown_core_element *element, markdown_core_block_hook hook) {
    markdown_core_block_gate ungated = {NULL};

    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return element->scan_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return element->interrupt_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return element->open_block_gate;
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
    case MARKDOWN_CORE_BLOCK_HOOK_PROBE:
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return ungated;
}

static bool S_element_implements_inline(const markdown_core_element *element, markdown_core_inline_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_INLINE_HOOK_INIT:
        return element->init_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_FINISH:
        return element->finish_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_DISPOSE:
        return element->dispose_inline != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_COUNT:
        break;
    }
    return false;
}

/* The owners each dialect-wide role resolves to. Each belongs to the last
 * element that declares it, in registration order, so a setup that registers
 * a replacement takes the role for its instance. */
static void S_resolve_owners(markdown_core_dialect *dialect) {
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        const markdown_core_element *element = instance->element;
        if (S_owns_document_lifecycle(element)) {
            dialect->document_structure = instance;
        }
        if (element->parse_text) {
            dialect->text_structure = instance;
        }
        if (element->open_text_block) {
            dialect->text_block_structure = instance;
        }
        if (element->delimiter_rule != MARKDOWN_CORE_DELIM_RULE_NONE) {
            dialect->delimiter_owners[element->delimiter_rule] = instance;
            if (element->delimiter_character) {
                dialect->delimiter_chars[element->delimiter_character] = element->delimiter_rule;
            }
        }
    }
    assert(dialect->document_structure && dialect->text_structure && dialect->text_block_structure);
}

/* THE PER-KIND RECORD (dialect.h, markdown_core_kind_record): one record
 * per kind index, the instance of the kind's structure element and facts
 * constant of that element (element.h), so the engine reads one record where
 * it would ask the kind table and then several descriptor fields. It is
 * projected after the instance table, which it resolves the structure in. */
static void S_project_kinds(markdown_core_dialect *dialect) {
    for (size_t index = 0; index < MARKDOWN_CORE_FINISH_KIND_COUNT; index++) {
        markdown_core_kind_record record = {NULL, NULL, 0};
        markdown_core_node_type kind =
            index < MARKDOWN_CORE_NODE_KIND_COUNT
                ? (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | index)
                : (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | (index - MARKDOWN_CORE_NODE_KIND_COUNT));
        const markdown_core_element *structure = markdown_core_structure_for_kind(kind);
        record.structure = structure ? markdown_core_dialect_instance(dialect, structure) : NULL;
        if (record.structure) {
            const struct {
                bool fact;
                unsigned flag;
            } facts[] = {
                {structure->inline_content, MARKDOWN_CORE_KIND_INLINES},
                {structure->contains_inlines_func != NULL, MARKDOWN_CORE_KIND_INLINES_ASK},
                {structure->deferred_inlines, MARKDOWN_CORE_KIND_DEFERRED},
                {structure->content_mode == MARKDOWN_CORE_CONTENT_LITERAL, MARKDOWN_CORE_KIND_LINES},
                {structure->accepts_lines_func != NULL, MARKDOWN_CORE_KIND_LINES_ASK},
                {structure->content_mode == MARKDOWN_CORE_CONTENT_PROSE, MARKDOWN_CORE_KIND_PROSE},
                {structure->paragraph, MARKDOWN_CORE_KIND_IS_PARAGRAPH},
                {structure->blank_opaque, MARKDOWN_CORE_KIND_BLANK_OPAQUE},
                {structure->blank_line != NULL, MARKDOWN_CORE_KIND_BLANK_ASK},
                {structure->blank_runs, MARKDOWN_CORE_KIND_BLANK_RUNS},
                {structure->propagates_child_blank, MARKDOWN_CORE_KIND_BLANK_PROPAGATES},
            };
            for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++) {
                if (facts[i].fact) {
                    record.flags |= (uint16_t)facts[i].flag;
                }
            }
            for (const markdown_core_node_type *open = structure->reopen_kinds; open && *open; open++) {
                if (*open == kind) {
                    record.flags |= MARKDOWN_CORE_KIND_REOPENS;
                }
            }
            record.complete = structure->complete_inline;
        }
        if (markdown_core_kind_owns_fields(kind)) {
            record.flags |= MARKDOWN_CORE_KIND_FIELDS;
        }
        dialect->kinds[index] = record;
    }
    /* The out-of-table index answers nothing; the storage is zeroed. */
}

/* THE INLINE BYTE TABLES. A byte ends a text run when an inline owner says
 * so (`terminates_text`), is looked through by flanking when one declares it
 * (`flanking_transparent`), and is offered to the owners that list it in
 * `dispatch`. These are properties of the registered elements alone, so they
 * are derived once, when the dialect is sealed -- never per inline pass.
 * They used to be installed around the inline pass and cleared after it, the
 * shape cmark-gfm needs because its tables are process globals; here they are
 * the instance's dialect's and die with it. */
static bool S_declared_earlier(const unsigned char *bytes, const unsigned char *at) {
    /* A byte written twice in one declaration is one declaration. The
     * declarations are a handful of bytes, so the check is the scan itself. */
    for (const unsigned char *p = bytes; p < at; p++) {
        if (*p == *at) {
            return true;
        }
    }
    return false;
}

/* Count the dispatch entries per byte into `offsets` as running totals and
 * return their sum, which the measure lays out after the dialect's struct. */
static size_t S_count_inline_dispatch(const markdown_core_element *const *elements, size_t count, size_t *offsets) {
    for (size_t i = 0; i < count; i++) {
        const markdown_core_element *element = elements[i];
        const unsigned char *bytes = (const unsigned char *)element->dispatch;
        for (const unsigned char *c = bytes; element->match_inline && c && *c; c++) {
            if (!S_declared_earlier(bytes, c)) {
                offsets[*c + 1]++;
            }
        }
    }
    for (size_t c = 0; c < 256; c++) {
        offsets[c + 1] += offsets[c];
    }
    return offsets[256];
}

static void S_project_inline_bytes(markdown_core_dialect *dialect) {
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        const markdown_core_element *element = instance->element;
        if (!element->match_inline && !element->insert_inline_from_delim) {
            continue;
        }
        /* A byte keeps a start predicate only while one element terminates
         * text at it: another owner's tokens are not that predicate's to
         * rule out, so a shared byte is unconditional. */
        for (const unsigned char *c = (const unsigned char *)element->terminates_text; c && *c; c++) {
            dialect->inline_start_owners[*c] =
                !dialect->special_chars[*c] && element->is_inline_start ? instance : NULL;
            dialect->special_chars[*c] = MARKDOWN_CORE_TEXT_END;
        }
        for (const unsigned char *c = (const unsigned char *)element->flanking_transparent; c && *c; c++) {
            dialect->skip_chars[*c] = 1;
        }
    }
}

/* Fill each byte's owners into `entries` in the order `try_elements` asks
 * them: by ascending precedence, and within one precedence in descriptor
 * order. The owners are ordered once -- a stable insertion by precedence over
 * the elements with an inline matcher -- and then emitted, so every byte's
 * list inherits that order with no per-byte sort. The ordering is total over
 * the field's values, not only the named ones, so every entry
 * `S_count_inline_dispatch` counted is written. */
static void S_project_inline_dispatch(markdown_core_dialect *dialect, const markdown_core_element_instance **entries) {
    const markdown_core_element_instance *owners[MARKDOWN_CORE_ELEMENT_LIMIT];
    size_t count = 0;
    assert(dialect->element_count <= MARKDOWN_CORE_ELEMENT_LIMIT);
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        if (!instance->element->match_inline) {
            continue;
        }
        size_t at = count++;
        while (at > 0 && owners[at - 1]->element->inline_precedence > instance->element->inline_precedence) {
            owners[at] = owners[at - 1];
            at--;
        }
        owners[at] = instance;
    }
    size_t next[256];
    memcpy(next, dialect->inline_dispatch_offsets, sizeof(next));
    dialect->inline_dispatch = entries;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *bytes = (const unsigned char *)owners[i]->element->dispatch;
        for (const unsigned char *c = bytes; c && *c; c++) {
            if (!S_declared_earlier(bytes, c)) {
                entries[next[*c]++] = owners[i];
            }
        }
    }
}

/* The (event, kind) keys a finish step's declaration projects to, counted
 * into `counts`: the EXIT of each kind it is asked at, and the ENTER and EXIT
 * of each kind whose extent it tracks. An element without a step projects to
 * none. Registration refused a step asked at no kind and a kind outside the
 * table (S_element_refused), so every key counted here is one the dispatch
 * indexes and none is the out-of-table key. Returns how many keys the element
 * added. */
static size_t S_count_finish_keys(const markdown_core_element *element, size_t *counts) {
    size_t keys = 0;
    if (!element->finish_step) {
        return 0;
    }
    for (const markdown_core_node_type *kind = element->finish_exit_kinds; kind && *kind; kind++) {
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind)]++;
        keys++;
    }
    for (const markdown_core_node_type *kind = element->finish_scope_kinds; kind && *kind; kind++) {
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_ENTER, *kind)]++;
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind)]++;
        keys += 2;
    }
    return keys;
}

/* Append `instance` under `key`, once: a kind written twice in one list is
 * one declaration, and the step is asked once per event. `acts_on` is the
 * element's declared acted-on kinds as a set and `gated` whether this entry
 * reads it (markdown_core_finish_step_entry). */
static void S_append_finish_step(const markdown_core_dialect *dialect, markdown_core_finish_step_entry **next,
                                 size_t key, const markdown_core_element_instance *instance, size_t slot,
                                 markdown_core_node_kind_set acts_on, bool gated) {
    if (next[key] != dialect->finish_dispatch[key] && next[key][-1].instance == instance) {
        return;
    }
    *next[key]++ = (markdown_core_finish_step_entry){instance, slot, acts_on, gated};
}

/* The finish steps by key: each declared key's list laid out in descriptor
 * order at `steps`, closed by a terminator, and one state slot per element
 * that projected anything, so the slot an entry names is always one the walk
 * keeps. The gate is the acted-on kinds as a set, read at an entry whose
 * asked-at kind is not one of them -- at one that is, the node being exited
 * is the proof the parse produced one. */
static void S_project_finish_steps(markdown_core_dialect *dialect, const size_t *key_counts,
                                   markdown_core_finish_step_entry *steps) {
    markdown_core_finish_step_entry *next[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t at = 0;
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        if (!key_counts[key]) {
            next[key] = NULL;
            continue;
        }
        dialect->finish_dispatch[key] = next[key] = steps + at;
        at += key_counts[key] + 1;
    }
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element_instance *instance = &dialect->instances[i];
        const markdown_core_element *element = instance->element;
        size_t slot = dialect->finish_step_slots;
        bool projected = false;
        if (!element->finish_step) {
            continue;
        }
        markdown_core_node_kind_set acts_on = {0, 0};
        for (const markdown_core_node_type *kind = element->finish_acts_on_kinds; kind && *kind; kind++) {
            markdown_core_node_kind_set_add(&acts_on, *kind);
        }
        for (const markdown_core_node_type *kind = element->finish_exit_kinds; kind && *kind; kind++) {
            markdown_core_node_kind_set asked = {0, 0};
            markdown_core_node_kind_set_add(&asked, *kind);
            bool gated = element->finish_acts_on_kinds && !markdown_core_node_kind_set_intersects(&acts_on, &asked);
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), instance,
                                 slot, acts_on, gated);
            projected = true;
        }
        for (const markdown_core_node_type *kind = element->finish_scope_kinds; kind && *kind; kind++) {
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_ENTER, *kind), instance,
                                 slot, acts_on, false);
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), instance,
                                 slot, acts_on, false);
            projected = true;
        }
        dialect->finish_step_slots += projected;
    }
    /* A kind written twice in one list was counted twice and appended once;
     * the terminator closes the list where it ends. The zeroed storage
     * already holds one past the counted end. */
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        if (next[key]) {
            *next[key] = (markdown_core_finish_step_entry){NULL, 0, {0, 0}, false};
        }
    }
}

/* THE INSTANCES: one per element, in element order. Each element's parse
 * record is given the next aligned offset in `state`, and its run record the
 * next in a run's block, so both are laid out in descriptor order; an element
 * that declares no parse record has none. Each instance is entered in the
 * table by descriptor, which registration made unique, and the table
 * measured at least twice the elements, so an entry is always empty and
 * every lookup ends. */
static void S_project_instances(markdown_core_dialect *dialect, const markdown_core_element *const *elements,
                                markdown_core_element_instance *instances, const markdown_core_element_instance **table,
                                size_t slots, unsigned char *state) {
    size_t state_at = 0;
    dialect->instances = instances;
    dialect->instance_table = table;
    dialect->instance_mask = slots - 1;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element *element = elements[i];
        markdown_core_element_instance *instance = &instances[i];
        instance->element = element;
        instance->state = element->state_size ? state + state_at : NULL;
        instance->run_offset = dialect->run_state_size;
        state_at += markdown_core_state_align(element->state_size);
        dialect->run_state_size += markdown_core_state_align(element->run_state_size);
        size_t at = markdown_core_instance_hash(element, dialect->instance_mask);
        while (table[at]) {
            assert(table[at]->element != element);
            at = (at + 1) & dialect->instance_mask;
        }
        table[at] = instance;
    }
}

/* THE PEERS: each instance's declared peers resolved, in declaration order,
 * to their instances in this dialect -- NULL for one the dialect does not
 * hold. Resolution happens here, once, so no parse-time code asks for
 * another element by name; the element that reads a peer's record decides
 * what an absent peer means. */
static void S_resolve_peers(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                            const markdown_core_element_instance **peers) {
    for (size_t i = 0; i < dialect->element_count; i++) {
        instances[i].peers = peers;
        for (const markdown_core_element *const *peer = instances[i].element->peers; peer && *peer; peer++) {
            *peers++ = markdown_core_dialect_instance(dialect, *peer);
        }
    }
}

/* THE LINE NAMES ITS CANDIDATES. Each gated family's owners are projected
 * into one list per key, in the family's own order: a count, then owner
 * indices, `count + 1` bytes, so a family's table is
 * `MARKDOWN_CORE_BLOCK_GATE_KEYS * (owners + 1)` bytes. An ungated owner is
 * on every byte's list; an indented line is decided by the indent bound
 * alone. */
static void S_project_gate_lists(markdown_core_dialect *dialect, const markdown_core_dialect_sizes *sizes,
                                 uint8_t *tables) {
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        if (!sizes->gated[hook]) {
            continue;
        }
        size_t owners = dialect->block_hook_counts[hook];
        size_t stride = owners + 1;
        uint8_t *table = tables;
        dialect->block_gate_lists[hook] = table;
        /* Counts and indices are bytes: registration bounds the dialect so
         * that they fit (MARKDOWN_CORE_ELEMENT_LIMIT). */
        assert(owners <= MARKDOWN_CORE_ELEMENT_LIMIT);
        for (size_t i = 0; i < owners; i++) {
            const markdown_core_element *element = dialect->block_hooks[hook][i]->element;
            markdown_core_block_gate gate = S_element_gate(element, (markdown_core_block_hook)hook);
            if (!gate.bytes) {
                for (size_t key = 0; key <= MARKDOWN_CORE_BLOCK_GATE_KEY_NONE; key++) {
                    uint8_t *list = table + key * stride;
                    list[1 + list[0]++] = (uint8_t)i;
                }
            } else {
                for (const unsigned char *c = (const unsigned char *)gate.bytes; *c; c++) {
                    uint8_t *list = table + *c * stride;
                    if (!list[0] || list[list[0]] != (uint8_t)i) {
                        list[1 + list[0]++] = (uint8_t)i;
                    }
                }
            }
            if (element->maximum_block_indent >= CODE_INDENT) {
                uint8_t *list = table + MARKDOWN_CORE_BLOCK_GATE_KEY_INDENTED * stride;
                list[1 + list[0]++] = (uint8_t)i;
            }
        }
        tables += MARKDOWN_CORE_BLOCK_GATE_KEYS * stride;
    }
}

/* MEASURE: what sealing the builder's dialect takes, counted from its element
 * list alone. A family is gated when any of its owners declares a gate: the
 * first declaration is what turns gating on, so an element that declares
 * nothing is never skipped. */
size_t markdown_core_dialect_measure(const markdown_core_dialect_builder *builder, markdown_core_dialect_sizes *sizes) {
    const markdown_core_element *const *elements = builder->elements;
    size_t count = builder->element_count;

    memset(sizes, 0, sizeof(*sizes));
    for (size_t i = 0; i < count; i++) {
        for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
            if (S_element_implements(elements[i], (markdown_core_block_hook)hook)) {
                sizes->block_totals[hook]++;
                sizes->pointers++;
                if (S_element_gate(elements[i], (markdown_core_block_hook)hook).bytes) {
                    sizes->gated[hook] = true;
                }
            }
        }
        for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
            if (S_element_implements_inline(elements[i], (markdown_core_inline_hook)hook)) {
                sizes->inline_totals[hook]++;
                sizes->pointers++;
            }
        }
        if (elements[i]->writes_below) {
            sizes->closed_writers++;
            sizes->pointers++;
        }
        sizes->steps += S_count_finish_keys(elements[i], sizes->finish_key_counts);
        sizes->state_bytes += markdown_core_state_align(elements[i]->state_size);
        sizes->run_state_bytes += markdown_core_state_align(elements[i]->run_state_size);
        for (const markdown_core_element *const *peer = elements[i]->peers; peer && *peer; peer++) {
            sizes->peers++;
        }
    }
    sizes->pointers += S_count_inline_dispatch(elements, count, sizes->inline_dispatch_offsets);
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        sizes->steps += sizes->finish_key_counts[key] != 0; /* the terminator */
    }
    /* The instance table: a power of two at least twice the elements, and
     * never empty, so a lookup always finds an empty entry to stop at. */
    for (sizes->instance_slots = 1; sizes->instance_slots < 2 * count; sizes->instance_slots *= 2) {
    }
    /* A family with no declared gate keeps no table and every owner is asked. */
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        if (sizes->gated[hook]) {
            sizes->gate_bytes += MARKDOWN_CORE_BLOCK_GATE_KEYS * (sizes->block_totals[hook] + 1);
        }
    }
    return sizes->steps * sizeof(markdown_core_finish_step_entry) + count * sizeof(markdown_core_element_instance) +
           (sizes->pointers + sizes->instance_slots + sizes->peers) * sizeof(const markdown_core_element_instance *) +
           sizes->gate_bytes;
}

/* SEAL: every table the dialect decides, projected once, into the storage
 * `sizes` was measured for. The tail after the struct holds the finish step
 * entries, then the instances, then the instance-pointer lists (the block
 * families, the inline-content families, the closed-block writers, the inline
 * dispatch), the
 * instance table and the resolved peers, then the gate tables. Each region's alignment is at most
 * the one before it, so each starts where the one before ends. The element
 * list is copied into the instances rather than taken, so the dialect owns
 * nothing apart from its storage and the builder still owns what it did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder, const markdown_core_dialect_sizes *sizes,
                                markdown_core_dialect *dialect, unsigned char *state) {
    size_t count = builder->element_count;
    markdown_core_finish_step_entry *step_entries = (markdown_core_finish_step_entry *)(dialect + 1);
    markdown_core_element_instance *instances = (markdown_core_element_instance *)(step_entries + sizes->steps);
    const markdown_core_element_instance **entries = (const markdown_core_element_instance **)(instances + count);
    const markdown_core_element_instance **table = entries + sizes->pointers;
    const markdown_core_element_instance **peers = table + sizes->instance_slots;
    uint8_t *tables = (uint8_t *)(peers + sizes->peers);

    dialect->element_count = count;
    S_project_instances(dialect, builder->elements, instances, table, sizes->instance_slots, state);
    assert(dialect->run_state_size == sizes->run_state_bytes);
    S_resolve_peers(dialect, instances, peers);
    S_resolve_owners(dialect);
    S_project_kinds(dialect);
    S_project_inline_bytes(dialect);
    /* What a container continuation may strip, as one table over the byte:
     * indentation, which every continuation strips, and the bytes each
     * container element declares for its own. */
    dialect->container_prefix[' '] = dialect->container_prefix['\t'] = true;
    for (size_t i = 0; i < count; i++) {
        const char *bytes = instances[i].element->container_prefix_bytes;
        for (const unsigned char *c = (const unsigned char *)bytes; bytes && *c; c++) {
            dialect->container_prefix[*c] = true;
        }
    }

    size_t at = 0;
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        dialect->block_hooks[hook] = entries + at;
        dialect->block_hook_counts[hook] = sizes->block_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_element_implements(instances[i].element, (markdown_core_block_hook)hook)) {
                entries[at++] = &instances[i];
            }
        }
    }
    for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
        dialect->inline_hooks[hook] = entries + at;
        dialect->inline_hook_counts[hook] = sizes->inline_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_element_implements_inline(instances[i].element, (markdown_core_inline_hook)hook)) {
                entries[at++] = &instances[i];
            }
        }
    }
    dialect->closed_writers = entries + at;
    dialect->closed_writer_count = sizes->closed_writers;
    for (size_t i = 0; i < count; i++) {
        if (instances[i].element->writes_below) {
            entries[at++] = &instances[i];
        }
        dialect->passes_declared |= instances[i].element->postprocess_func != NULL;
    }
    memcpy(dialect->inline_dispatch_offsets, sizes->inline_dispatch_offsets, sizeof(dialect->inline_dispatch_offsets));
    S_project_inline_dispatch(dialect, entries + at);
    at += dialect->inline_dispatch_offsets[256];
    assert(at == sizes->pointers);

    S_project_finish_steps(dialect, sizes->finish_key_counts, step_entries);
    S_project_gate_lists(dialect, sizes, tables);
}
