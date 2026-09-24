#include <assert.h>
#include <string.h>

#include "alloc.h"
#include "block_internal.h"
#include "dialect.h"

/* Why a descriptor is refused, or NULL. The rule: an element takes part in
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
 * none. */
static bool S_finish_kind_indexable(markdown_core_node_type kind) {
    unsigned class = (unsigned)kind & MARKDOWN_CORE_NODE_TYPE_MASK;
    return (class == MARKDOWN_CORE_NODE_TYPE_BLOCK || class == MARKDOWN_CORE_NODE_TYPE_INLINE) &&
           ((unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK) < MARKDOWN_CORE_NODE_KIND_COUNT;
}

static bool S_owns_document_lifecycle(const markdown_core_element *element) {
    return element->init_document && element->dispose_document && element->read_document_prefix &&
           element->prepare_document && element->finish_document && element->open_text_block;
}

static const char *S_element_rejection(const markdown_core_element *element) {
    if (element->finish_step && element->postprocess_func) {
        return "declares both a finish step and a postprocess pass";
    }
    if ((element->finish_exit_kinds || element->finish_scope_kinds) && !element->finish_step) {
        return "declares where a finish step is asked without a finish step";
    }
    if (element->finish_step) {
        const markdown_core_node_type *lists[] = {element->finish_exit_kinds, element->finish_scope_kinds};
        bool asked = false;
        for (size_t list = 0; list < 2; list++) {
            for (const markdown_core_node_type *kind = lists[list]; kind && *kind; kind++) {
                if (!S_finish_kind_indexable(*kind)) {
                    return "declares a finish step at a kind outside the kind table";
                }
                asked = true;
            }
        }
        if (!asked) {
            return "declares a finish step asked at no kind";
        }
    }
    for (const markdown_core_node_type *exit = element->finish_exit_kinds; exit && *exit; exit++) {
        for (const markdown_core_node_type *scope = element->finish_scope_kinds; scope && *scope; scope++) {
            if (*exit == *scope) {
                return "declares a kind as both a finish exit kind and a finish scope kind";
            }
        }
    }
    if ((element->init_document || element->dispose_document || element->read_document_prefix ||
         element->prepare_document || element->finish_document || element->observe_inline ||
         element->open_text_block) &&
        !S_owns_document_lifecycle(element)) {
        return "declares only part of the document lifecycle";
    }
    return NULL;
}

void markdown_core_dialect_builder_init(markdown_core_dialect_builder *builder,
                                        const markdown_core_element *const *elements, size_t count) {
#ifndef NDEBUG
    for (size_t i = 0; i < count; i++) {
        assert(!S_element_rejection(elements[i]));
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
    if (S_element_rejection(element) || count >= MARKDOWN_CORE_ELEMENT_LIMIT) {
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
        const markdown_core_element *element = dialect->elements[i];
        if (S_owns_document_lifecycle(element)) {
            dialect->document_structure = element;
        }
        if (element->parse_text) {
            dialect->text_structure = element;
        }
        if (element->delimiter_rule != MARKDOWN_CORE_DELIM_RULE_NONE) {
            dialect->delimiter_owners[element->delimiter_rule] = element;
            if (element->delimiter_character) {
                dialect->delimiter_chars[element->delimiter_character] = element->delimiter_rule;
            }
        }
    }
    assert(dialect->document_structure && dialect->text_structure);
}

/* THE WALK'S PER-KIND RECORD (dialect.h, markdown_core_finish_kind): one
 * record per kind index, each fact a constant of the kind's structure
 * element (element.h). Projected beside the step lists so that the walk
 * reads one record where it asked three descriptor fields per event. */
static void S_project_finish_kinds(markdown_core_dialect *dialect) {
    for (size_t index = 0; index < MARKDOWN_CORE_FINISH_KIND_COUNT; index++) {
        markdown_core_finish_kind record = {NULL, 0};
        markdown_core_node_type kind =
            index < MARKDOWN_CORE_NODE_KIND_COUNT
                ? (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | index)
                : (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | (index - MARKDOWN_CORE_NODE_KIND_COUNT));
        const markdown_core_element *structure = markdown_core_structure_for_kind(kind);
        if (structure) {
            if (structure->inline_content || structure->contains_inlines_func) {
                record.flags |= MARKDOWN_CORE_FINISH_KIND_PARSES;
            }
            if (structure->deferred_inlines) {
                record.flags |= MARKDOWN_CORE_FINISH_KIND_DEFERRED;
            }
            record.complete = structure->complete_inline;
        }
        if (markdown_core_kind_owns_fields(kind)) {
            record.flags |= MARKDOWN_CORE_FINISH_KIND_FIELDS;
        }
        dialect->finish_kinds[index] = record;
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
        const markdown_core_element *element = dialect->elements[i];
        if (!element->match_inline && !element->insert_inline_from_delim) {
            continue;
        }
        /* A byte several owners terminate keeps a start predicate only while
         * they all agree on it; disagreement leaves the byte unconditional. */
        for (const unsigned char *c = (const unsigned char *)element->terminates_text; c && *c; c++) {
            if (!dialect->special_chars[*c]) {
                dialect->inline_start_predicates[*c] = element->is_inline_start;
            } else if (dialect->inline_start_predicates[*c] != element->is_inline_start) {
                dialect->inline_start_predicates[*c] = NULL;
            }
            dialect->special_chars[*c] = 1;
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
static void S_project_inline_dispatch(markdown_core_dialect *dialect, const markdown_core_element **entries) {
    const markdown_core_element *owners[MARKDOWN_CORE_ELEMENT_LIMIT];
    size_t count = 0;
    assert(dialect->element_count <= MARKDOWN_CORE_ELEMENT_LIMIT);
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element *element = dialect->elements[i];
        if (!element->match_inline) {
            continue;
        }
        size_t at = count++;
        while (at > 0 && owners[at - 1]->inline_precedence > element->inline_precedence) {
            owners[at] = owners[at - 1];
            at--;
        }
        owners[at] = element;
    }
    size_t next[256];
    memcpy(next, dialect->inline_dispatch_offsets, sizeof(next));
    dialect->inline_dispatch = entries;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *bytes = (const unsigned char *)owners[i]->dispatch;
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
 * table (S_element_rejection), so every key counted here is one the dispatch
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

/* Append `element` under `key`, once: a kind written twice in one list is
 * one declaration, and the step is asked once per event. `acts_on` is the
 * element's declared acted-on kinds as a set and `gated` whether this entry
 * reads it (markdown_core_finish_step_entry). */
static void S_append_finish_step(const markdown_core_dialect *dialect, markdown_core_finish_step_entry **next,
                                 size_t key, const markdown_core_element *element, size_t slot,
                                 markdown_core_node_kind_set acts_on, bool gated) {
    if (next[key] != dialect->finish_dispatch[key] && next[key][-1].element == element) {
        return;
    }
    *next[key]++ = (markdown_core_finish_step_entry){element, slot, acts_on, gated};
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
        const markdown_core_element *element = dialect->elements[i];
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
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), element,
                                 slot, acts_on, gated);
            projected = true;
        }
        for (const markdown_core_node_type *kind = element->finish_scope_kinds; kind && *kind; kind++) {
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_ENTER, *kind), element,
                                 slot, acts_on, false);
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), element,
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

/* THE LINE NAMES ITS CANDIDATES. Each gated family's owners are projected
 * into one list per key, in the family's own order: a count, then owner
 * indices, `count + 1` bytes, so a family's table is
 * `MARKDOWN_CORE_BLOCK_GATE_KEYS * (owners + 1)` bytes. An ungated owner is
 * on every byte's list; an indented line is decided by the indent bound
 * alone. */
static void S_project_gate_lists(markdown_core_dialect *dialect, const markdown_core_dialect_layout *layout,
                                 uint8_t *tables) {
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        if (!layout->gated[hook]) {
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
            const markdown_core_element *element = dialect->block_hooks[hook][i];
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
size_t markdown_core_dialect_measure(const markdown_core_dialect_builder *builder,
                                     markdown_core_dialect_layout *layout) {
    const markdown_core_element *const *elements = builder->elements;
    size_t count = builder->element_count;

    memset(layout, 0, sizeof(*layout));
    layout->pointers = count;
    for (size_t i = 0; i < count; i++) {
        for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
            if (S_element_implements(elements[i], (markdown_core_block_hook)hook)) {
                layout->block_totals[hook]++;
                layout->pointers++;
                if (S_element_gate(elements[i], (markdown_core_block_hook)hook).bytes) {
                    layout->gated[hook] = true;
                }
            }
        }
        for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
            if (S_element_implements_inline(elements[i], (markdown_core_inline_hook)hook)) {
                layout->inline_totals[hook]++;
                layout->pointers++;
            }
        }
        layout->steps += S_count_finish_keys(elements[i], layout->finish_key_counts);
    }
    layout->pointers += S_count_inline_dispatch(elements, count, layout->inline_dispatch_offsets);
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        layout->steps += layout->finish_key_counts[key] != 0; /* the terminator */
    }
    /* A family with no declared gate keeps no table and every owner is asked. */
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        if (layout->gated[hook]) {
            layout->gate_bytes += MARKDOWN_CORE_BLOCK_GATE_KEYS * (layout->block_totals[hook] + 1);
        }
    }
    return layout->pointers * sizeof(const markdown_core_element *) +
           layout->steps * sizeof(markdown_core_finish_step_entry) + layout->gate_bytes;
}

/* SEAL: every table the dialect decides, projected once, into the storage
 * the layout was measured for. The tail after the struct holds the
 * element-pointer lists (the element list itself, the block families, the
 * inline-content families, the inline dispatch), then the finish step
 * entries, then the gate tables. The first two regions are pointer-aligned
 * and start where the one before ends; the tables are bytes. The element list
 * is copied rather than taken, so the dialect owns nothing apart from its
 * storage and the builder still owns what it did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder,
                                const markdown_core_dialect_layout *layout, markdown_core_dialect *dialect) {
    size_t count = builder->element_count;
    const markdown_core_element **entries = (const markdown_core_element **)(dialect + 1);
    markdown_core_finish_step_entry *step_entries = (markdown_core_finish_step_entry *)(entries + layout->pointers);
    uint8_t *tables = (uint8_t *)(step_entries + layout->steps);

    if (count) {
        memcpy(entries, builder->elements, count * sizeof(*entries));
    }
    dialect->elements = entries;
    dialect->element_count = count;
    size_t at = count;

    S_resolve_owners(dialect);
    S_project_finish_kinds(dialect);
    S_project_inline_bytes(dialect);
    /* What a container continuation may strip, as one table over the byte:
     * indentation, which every continuation strips, and the bytes each
     * container element declares for its own. */
    dialect->container_prefix[' '] = dialect->container_prefix['\t'] = true;
    for (size_t i = 0; i < count; i++) {
        const char *bytes = dialect->elements[i]->container_prefix_bytes;
        for (const unsigned char *c = (const unsigned char *)bytes; bytes && *c; c++) {
            dialect->container_prefix[*c] = true;
        }
    }

    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        dialect->block_hooks[hook] = entries + at;
        dialect->block_hook_counts[hook] = layout->block_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_element_implements(dialect->elements[i], (markdown_core_block_hook)hook)) {
                entries[at++] = dialect->elements[i];
            }
        }
    }
    for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
        dialect->inline_hooks[hook] = entries + at;
        dialect->inline_hook_counts[hook] = layout->inline_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_element_implements_inline(dialect->elements[i], (markdown_core_inline_hook)hook)) {
                entries[at++] = dialect->elements[i];
            }
        }
    }
    memcpy(dialect->inline_dispatch_offsets, layout->inline_dispatch_offsets, sizeof(dialect->inline_dispatch_offsets));
    S_project_inline_dispatch(dialect, entries + at);
    at += dialect->inline_dispatch_offsets[256];
    assert(at == layout->pointers);

    S_project_finish_steps(dialect, layout->finish_key_counts, step_entries);
    S_project_gate_lists(dialect, layout, tables);
}
