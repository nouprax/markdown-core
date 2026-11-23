#include <assert.h>
#include <string.h>

#include "alloc.h"
#include "block_internal.h"
#include "dialect.h"
#include "roster.h"

/* Whether a descriptor is refused. The rule: an element takes part in the
 * finish stage through one step, asked in completion's walk where it can
 * keep its contract (markdown-core-element-api.h states it). Completion takes
 * the blocks in the order they closed, so the blocks below a block complete
 * before it and its ENTER and EXIT bracket none of their events: a block kind
 * as a scope kind would track an extent completion never walks, and is
 * refused. A step is asked once per event, so a kind in both of its lists --
 * one EXIT declared twice -- is refused rather than delivered twice. A step
 * asked at no kind would never be called, and is refused rather than
 * silently kept. And a kind the dispatch table cannot index -- an ordinal at
 * or past MARKDOWN_CORE_NODE_KIND_COUNT, or a value of neither class -- would
 * share the table's one out-of-table key and be asked at every such node's
 * events instead of its own, so it is refused too: that key is never
 * declared.
 *
 * The document lifecycle is one concern too. The engine calls every one of
 * its hooks without asking, on whichever element owns it, so an element that
 * declares part of it would have the engine call through a NULL the moment it
 * became the owner: it declares all of them or none.

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
    const markdown_core_document_ops *document = element->document;
    return document && document->init && document->read && document->publish && document->candidate &&
           document->reopen && document->splice && MARKDOWN_CORE_NODE_TYPE_BLOCK_P(document->kind) &&
           S_finish_kind_indexable(document->kind);
}

/* The completion's hooks of an element that declares none (the Null Object
 * the projections read in their place). */
static const markdown_core_finish_ops S_NO_FINISH = {0};

static inline const markdown_core_finish_ops *S_finish_of(const markdown_core_element *element) {
    return element->finish ? element->finish : &S_NO_FINISH;
}

/* The content of an element that declares none: a container of blocks. */
static const markdown_core_content_ops S_NO_CONTENT = {0};

static inline const markdown_core_content_ops *S_content_of(const markdown_core_element *element) {
    return element->content ? element->content : &S_NO_CONTENT;
}

/* The inline parse of an element that takes no part in it. */
static const markdown_core_inline_ops S_NO_INLINES = {0};

static inline const markdown_core_inline_ops *S_inlines_of(const markdown_core_element *element) {
    return element->inlines ? element->inlines : &S_NO_INLINES;
}

/* The opening of an element none of whose blocks opens on a line. */
static const markdown_core_open_ops S_NO_OPENS = {0};

static inline const markdown_core_open_ops *S_opens_of(const markdown_core_element *element) {
    return element->opens ? element->opens : &S_NO_OPENS;
}

/* The lines of an element whose blocks take none after their first: the
 * object every instance's `lines` points at when its element has none. */
static const markdown_core_line_ops S_NO_LINES = {0};

static inline const markdown_core_line_ops *S_lines_of(const markdown_core_element *element) {
    return element->lines ? element->lines : &S_NO_LINES;
}

static bool S_element_refused(const markdown_core_element *element) {
    const markdown_core_finish_ops *finish = S_finish_of(element);
    /* Kinds to ask a finish step at, without a finish step. */
    if ((finish->exits || finish->scopes) && !finish->step) {
        return true;
    }
    /* A finish step at a kind outside the kind table, or asked at no kind. */
    if (finish->step) {
        const markdown_core_node_type *lists[] = {finish->exits, finish->scopes};
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
    /* A block kind as a finish scope kind, whose extent completion never
     * walks. */
    for (const markdown_core_node_type *scope = finish->scopes; scope && *scope; scope++) {
        if (MARKDOWN_CORE_NODE_TYPE_BLOCK_P(*scope)) {
            return true;
        }
    }
    /* A kind as both a finish exit kind and a finish scope kind. */
    for (const markdown_core_node_type *exit = finish->exits; exit && *exit; exit++) {
        for (const markdown_core_node_type *scope = finish->scopes; scope && *scope; scope++) {
            if (*exit == *scope) {
                return true;
            }
        }
    }
    /* A fold hook asked at no kind, kinds without one to ask, or a kind to
     * ask it at that is not a block kind of the element's structure. */
    const markdown_core_fold_ops *fold = element->fold;
    if (fold && !fold->child != !(fold->kinds && *fold->kinds)) {
        return true;
    }
    for (const markdown_core_node_type *kind = fold ? fold->kinds : NULL; kind && *kind; kind++) {
        if (!MARKDOWN_CORE_NODE_TYPE_BLOCK_P(*kind) || markdown_core_structure_for_kind(*kind) != element) {
            return true;
        }
    }
    /* Only part of the document lifecycle. */
    if (element->document && !S_owns_document_lifecycle(element)) {
        return true;
    }
    /* Plain text made of a kind that is not an inline kind of the table. */
    const markdown_core_inline_ops *inlines = S_inlines_of(element);
    if (inlines->text &&
        !(MARKDOWN_CORE_NODE_TYPE_INLINE_P(inlines->plain) && S_finish_kind_indexable(inlines->plain))) {
        return true;
    }
    /* A flanking-transparent byte outside ASCII. */
    for (const unsigned char *c = (const unsigned char *)S_inlines_of(element)->transparent; c && *c; c++) {
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

/* Whether `a` and `b` are one space: a space is its name (element.h,
 * markdown_core_space). */
static bool S_same_space(const markdown_core_space *a, const markdown_core_space *b) {
    return a == b || strcmp(a->name, b->name) == 0;
}

/* Whether two of the spaces the `count` elements name are one space that
 * folds in one and not the other, which no dialect holds. */
static bool S_spaces_disagree(const markdown_core_element *const *elements, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (const markdown_core_space *const *space = elements[i]->spaces; space && *space; space++) {
            for (size_t e = 0; e <= i; e++) {
                for (const markdown_core_space *const *other = elements[e]->spaces; other && *other; other++) {
                    if (S_same_space(*other, *space) && (*other)->folds != (*space)->folds) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/* The distinct spaces the `count` elements name, and, past `limit`, any
 * number over it. */
static size_t S_count_spaces(const markdown_core_element *const *elements, size_t count, size_t limit) {
    size_t distinct = 0;
    for (size_t i = 0; i < count && distinct <= limit; i++) {
        for (const markdown_core_space *const *space = elements[i]->spaces; space && *space; space++) {
            bool seen = false;
            for (size_t e = 0; e <= i && !seen; e++) {
                for (const markdown_core_space *const *other = elements[e]->spaces; other && *other; other++) {
                    if (e == i && other == space) {
                        break;
                    }
                    if (S_same_space(*other, *space)) {
                        seen = true;
                        break;
                    }
                }
            }
            distinct += !seen;
        }
    }
    return distinct;
}

void markdown_core_dialect_builder_init(markdown_core_dialect_builder *builder,
                                        const markdown_core_element *const *elements, size_t count) {
#ifndef NDEBUG
    for (size_t i = 0; i < count; i++) {
        assert(!S_element_refused(elements[i]) && !S_element_attached(elements, i, elements[i]));
    }
    assert(!S_spaces_disagree(elements, count));
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
    /* A dialect numbers its spaces in a byte (MARKDOWN_CORE_SPACE_LIMIT). */
    if (element->spaces && element->spaces[0]) {
        const markdown_core_element *probe[MARKDOWN_CORE_ELEMENT_LIMIT];
        for (size_t i = 0; i < count; i++) {
            probe[i] = builder->elements[i];
        }
        probe[count] = element;
        if (S_count_spaces(probe, count + 1, MARKDOWN_CORE_SPACE_LIMIT) > MARKDOWN_CORE_SPACE_LIMIT ||
            S_spaces_disagree(probe, count + 1)) {
            return 0;
        }
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

/* Whether an element whose opening is `opens` implements `hook`. */
static bool S_opens_implement(const markdown_core_open_ops *opens, markdown_core_block_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return opens->scan != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return opens->interrupt != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return opens->open != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
        return opens->last != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_CUT:
        return opens->cut != NULL;
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return false;
}

/* The gate the opening `opens` declares for `hook`, or an ungated one. A
 * family gains a gate by adding a field to the opening and a case here; the dispatcher never
 * learns an element's name. */
static markdown_core_block_gate S_opens_gate(const markdown_core_open_ops *opens, markdown_core_block_hook hook) {
    markdown_core_block_gate ungated = {NULL};

    switch (hook) {
    case MARKDOWN_CORE_BLOCK_HOOK_SCAN:
        return opens->scan_needs;
    case MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT:
        return opens->interrupt_needs;
    case MARKDOWN_CORE_BLOCK_HOOK_OPEN:
        return opens->open_needs;
    case MARKDOWN_CORE_BLOCK_HOOK_CUT:
        /* Asked as a block closes, not per line: its gate is the byte set
         * `fronts` (dialect.h). */
    case MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH:
    case MARKDOWN_CORE_BLOCK_HOOK_COUNT:
        break;
    }
    return ungated;
}

static bool S_inlines_implement(const markdown_core_inline_ops *inlines, markdown_core_inline_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_INLINE_HOOK_INIT:
        return inlines->init != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_FINISH:
        return inlines->finish != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_DISPOSE:
        return inlines->dispose != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_OPEN:
        return inlines->holds != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_REPORT:
        return inlines->report != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_OPENED:
        return inlines->opened != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_SETTLED:
        return inlines->settled != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_FREED:
        return inlines->freed != NULL;
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
        if (S_inlines_of(element)->text) {
            dialect->text_structure = instance;
        }
        if (S_opens_of(element)->text) {
            dialect->text_block_structure = instance;
        }
        if (element->spelling) {
            dialect->spelling = element->spelling;
        }
        if (S_inlines_of(element)->rule != MARKDOWN_CORE_DELIM_RULE_NONE) {
            dialect->delimiter_owners[S_inlines_of(element)->rule] = instance;
            if (S_inlines_of(element)->character) {
                dialect->delimiter_chars[S_inlines_of(element)->character] = S_inlines_of(element)->rule;
            }
        }
    }
    assert(dialect->document_structure && dialect->text_structure && dialect->text_block_structure);
    dialect->root = dialect->document_structure->element->document->kind;
    dialect->plain = S_inlines_of(dialect->text_structure->element)->plain;
    dialect->plain_index = markdown_core_finish_kind_index(dialect->plain);
    assert(MARKDOWN_CORE_NODE_TYPE_BLOCK_P(dialect->root) && MARKDOWN_CORE_NODE_TYPE_INLINE_P(dialect->plain));
}

/* THE PER-KIND RECORD (dialect.h, markdown_core_kind_record): one record
 * per kind index, the instance of the kind's structure element and facts
 * constant of that element (element.h), so the engine reads one record where
 * it would ask the kind table and then several descriptor fields. It is
 * projected after the instance table, which it resolves the structure in. */
static void S_project_kinds(markdown_core_dialect *dialect) {
    for (size_t index = 0; index < MARKDOWN_CORE_FINISH_KIND_COUNT; index++) {
        markdown_core_kind_record record = {0};
        markdown_core_node_type kind =
            index < MARKDOWN_CORE_NODE_KIND_COUNT
                ? (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_BLOCK | index)
                : (markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | (index - MARKDOWN_CORE_NODE_KIND_COUNT));
        const markdown_core_element *structure = markdown_core_structure_for_kind(kind);
        record.structure = structure ? markdown_core_dialect_instance(dialect, structure) : NULL;
        if (record.structure) {
            const markdown_core_content_ops *content = S_content_of(structure);
            const markdown_core_line_ops *lines = S_lines_of(structure);
            /* Lines are a block's content: an element's inline kinds take
             * none, whatever its blocks take. */
            const bool block = index < MARKDOWN_CORE_NODE_KIND_COUNT;
            record.flags =
                (uint16_t)((content->inlines ? MARKDOWN_CORE_KIND_INLINES : 0) |
                           (content->holds_inlines ? MARKDOWN_CORE_KIND_INLINES_ASK : 0) |
                           (S_finish_of(structure)->deferred ? MARKDOWN_CORE_KIND_DEFERRED : 0) |
                           (block && content->mode == MARKDOWN_CORE_CONTENT_LITERAL ? MARKDOWN_CORE_KIND_LINES : 0) |
                           (content->holds_lines ? MARKDOWN_CORE_KIND_LINES_ASK : 0) |
                           (content->mode == MARKDOWN_CORE_CONTENT_PROSE ? MARKDOWN_CORE_KIND_PROSE : 0) |
                           (content->paragraph ? MARKDOWN_CORE_KIND_IS_PARAGRAPH : 0) |
                           (lines->blank_opaque ? MARKDOWN_CORE_KIND_BLANK_OPAQUE : 0) |
                           (lines->blank_last ? MARKDOWN_CORE_KIND_BLANK_ASK : 0) |
                           (lines->blank_runs ? MARKDOWN_CORE_KIND_BLANK_RUNS : 0) |
                           (lines->blank_spreads ? MARKDOWN_CORE_KIND_BLANK_PROPAGATES : 0));
            record.complete = S_finish_of(structure)->complete;
        }
        const markdown_core_record_ops *fields = markdown_core_record_for_kind(kind);
        if (fields->visit && kind != dialect->root) {
            record.flags |= MARKDOWN_CORE_KIND_FIELDS;
        }
        dialect->kinds[index] = record;
    }
    /* The nodes each element follows, and the kinds its following makes
     * (element.h, markdown_core_follow). */
    for (size_t i = 0; i < dialect->element_count; i++) {
        for (const markdown_core_follow *follow = dialect->instances[i].element->follows; follow && follow->kinds;
             follow++) {
            for (const markdown_core_node_type *kind = follow->kinds; *kind != MARKDOWN_CORE_NODE_NONE; kind++) {
                markdown_core_kind_record *record = &dialect->kinds[markdown_core_finish_kind_index(*kind)];
                if (follow->closed) {
                    record->flags |= MARKDOWN_CORE_KIND_TRAILED;
                }
                /* At a line, which the engine asks no one at, the record keeps
                 * the follower of a closed leader: an edit asks it which lines
                 * write one (`writes`). */
                if (follow->at != MARKDOWN_CORE_FOLLOW_LINE || follow->closed) {
                    record->follower[follow->at] = &dialect->instances[i];
                    record->follow[follow->at] = follow;
                }
            }
            if (follow->makes != MARKDOWN_CORE_NODE_NONE) {
                markdown_core_kind_record *record = &dialect->kinds[markdown_core_finish_kind_index(follow->makes)];
                record->maker = &dialect->instances[i];
                record->made = follow;
            }
        }
    }
    /* A structure's fold hook is asked at the kinds it lists, each a block
     * kind it is the structure of (S_element_refused). */
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_fold_ops *fold = dialect->instances[i].element->fold;
        for (const markdown_core_node_type *kind = fold ? fold->kinds : NULL; kind && *kind; kind++) {
            dialect->kinds[markdown_core_block_kind_index(*kind)].flags |= MARKDOWN_CORE_KIND_FOLDS;
        }
        const markdown_core_edit_ops *edit = dialect->instances[i].element->edit;
        for (const markdown_core_node_type *kind = edit ? edit->awaits : NULL; kind && *kind; kind++) {
            dialect->kinds[markdown_core_block_kind_index(*kind)].flags |= MARKDOWN_CORE_KIND_AWAITS;
        }
        for (const markdown_core_node_type *kind = edit ? edit->joins : NULL; kind && *kind; kind++) {
            dialect->kinds[markdown_core_block_kind_index(*kind)].flags |= MARKDOWN_CORE_KIND_JOINS;
        }
    }
    /* The out-of-table index answers nothing; the storage is zeroed. */
}

/* THE INLINE BYTE TABLES. A byte ends a text run when an inline owner says
 * so (`terminates`), is looked through by flanking when one declares it
 * (`transparent`), and is offered to the owners that list it in
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
        const unsigned char *bytes = (const unsigned char *)S_inlines_of(element)->dispatch;
        for (const unsigned char *c = bytes; S_inlines_of(element)->match && c && *c; c++) {
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
        if (!S_inlines_of(element)->match && !S_inlines_of(element)->insert) {
            continue;
        }
        /* A byte keeps a start predicate only while one element terminates
         * text at it: another owner's tokens are not that predicate's to
         * rule out, so a shared byte is unconditional. */
        for (const unsigned char *c = (const unsigned char *)S_inlines_of(element)->terminates; c && *c; c++) {
            dialect->inline_start_owners[*c] =
                !dialect->special_chars[*c] && S_inlines_of(element)->starts ? instance : NULL;
            dialect->special_chars[*c] = MARKDOWN_CORE_TEXT_END;
        }
        for (const unsigned char *c = (const unsigned char *)S_inlines_of(element)->transparent; c && *c; c++) {
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
        if (!S_inlines_of(instance->element)->match) {
            continue;
        }
        size_t at = count++;
        while (at > 0 &&
               S_inlines_of(owners[at - 1]->element)->precedence > S_inlines_of(instance->element)->precedence) {
            owners[at] = owners[at - 1];
            at--;
        }
        owners[at] = instance;
    }
    size_t next[256];
    memcpy(next, dialect->inline_dispatch_offsets, sizeof(next));
    dialect->inline_dispatch = entries;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *bytes = (const unsigned char *)S_inlines_of(owners[i]->element)->dispatch;
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
    const markdown_core_finish_ops *finish = S_finish_of(element);
    size_t keys = 0;
    if (!finish->step) {
        return 0;
    }
    for (const markdown_core_node_type *kind = finish->exits; kind && *kind; kind++) {
        counts[markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind)]++;
        keys++;
    }
    for (const markdown_core_node_type *kind = finish->scopes; kind && *kind; kind++) {
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
        const markdown_core_finish_ops *finish = S_finish_of(instance->element);
        size_t slot = dialect->finish_step_slots;
        bool projected = false;
        if (!finish->step) {
            continue;
        }
        markdown_core_node_kind_set acts_on = {0, 0};
        for (const markdown_core_node_type *kind = finish->acts; kind && *kind; kind++) {
            markdown_core_node_kind_set_add(&acts_on, *kind);
        }
        for (const markdown_core_node_type *kind = finish->exits; kind && *kind; kind++) {
            markdown_core_node_kind_set asked = {0, 0};
            markdown_core_node_kind_set_add(&asked, *kind);
            bool gated = finish->acts && !markdown_core_node_kind_set_intersects(&acts_on, &asked);
            S_append_finish_step(dialect, next, markdown_core_finish_key(MARKDOWN_CORE_EVENT_EXIT, *kind), instance,
                                 slot, acts_on, gated);
            projected = true;
        }
        for (const markdown_core_node_type *kind = finish->scopes; kind && *kind; kind++) {
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
                                unsigned bits, unsigned char *state) {
    const size_t mask = ((size_t)1 << bits) - 1;
    size_t state_at = 0;
    dialect->instances = instances;
    dialect->instance_table = table;
    dialect->instance_bits = bits;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_element *element = elements[i];
        markdown_core_element_instance *instance = &instances[i];
        instance->element = element;
        instance->lines = S_lines_of(element);
        instance->state = element->state_size ? state + state_at : NULL;
        instance->run_offset = dialect->run_state_size;
        state_at += markdown_core_state_align(element->state_size);
        dialect->run_state_size += markdown_core_state_align(element->run_state_size);
        instance->bracket_offset = dialect->bracket_size;
        dialect->bracket_size += markdown_core_state_align(element->bracket_size);
        const size_t candidate = element->candidate ? markdown_core_state_align(element->candidate->size) : 0;
        if (dialect->candidate_state_size < candidate) {
            dialect->candidate_state_size = candidate;
        }
        size_t at = markdown_core_instance_hash(element, bits);
        while (table[at]) {
            assert(table[at]->element != element);
            at = (at + 1) & mask;
        }
        table[at] = instance;
    }
}

/* THE NEEDS: each service an instance asks resolved, in declaration order,
 * to the last instance of this dialect that provides it and that element's
 * operations -- both NULL when none does, as an owner of a role is the
 * last element that declares it (S_resolve_owners), so a setup that
 * registers a replacement provides the service for it. Resolution happens
 * here, once, so no parse-time code asks for a service by name; the element
 * that asks decides what an absent one means. */
static const void *S_provided(const markdown_core_element *element, const char *name) {
    for (const markdown_core_service *service = element->provides; service && service->name; service++) {
        if (strcmp(service->name, name) == 0) {
            return service->ops;
        }
    }
    return NULL;
}

static void S_resolve_needs(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                            markdown_core_need *needs) {
    for (size_t i = 0; i < dialect->element_count; i++) {
        instances[i].needs = needs;
        for (const char *const *name = instances[i].element->needs; name && *name; name++) {
            *needs = (markdown_core_need){0};
            for (size_t e = dialect->element_count; e-- > 0;) {
                const void *ops = S_provided(instances[e].element, *name);
                if (ops) {
                    *needs = (markdown_core_need){&instances[e], ops};
                    break;
                }
            }
            needs++;
        }
    }
}

/* THE SPACES: each space the instances name numbered once, after the
 * engine's ANCHOR, in descriptor order of first naming, and each
 * instance's namings resolved to the numbers, in its order. */
static void S_number_spaces(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                            const markdown_core_space **spaces, unsigned *namings) {
    size_t kinds = MARKDOWN_CORE_ROSTER_ANCHOR + 1;
    spaces[0] = spaces[MARKDOWN_CORE_ROSTER_ANCHOR] = NULL;
    for (size_t i = 0; i < dialect->element_count; i++) {
        instances[i].spaces = namings;
        for (const markdown_core_space *const *space = instances[i].element->spaces; space && *space; space++) {
            size_t kind = MARKDOWN_CORE_ROSTER_ANCHOR + 1;
            while (kind < kinds && !S_same_space(spaces[kind], *space)) {
                kind++;
            }
            if (kind == kinds) {
                spaces[kinds++] = *space;
            }
            *namings++ = (unsigned)kind;
        }
    }
    dialect->spaces = spaces;
    dialect->space_count = kinds;
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
            /* An owner of the family implements the opening. */
            const markdown_core_open_ops *opens = dialect->block_hooks[hook][i]->element->opens;
            markdown_core_block_gate gate = S_opens_gate(opens, (markdown_core_block_hook)hook);
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
            if (opens->indent >= CODE_INDENT) {
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
        const markdown_core_open_ops *opens = S_opens_of(elements[i]);
        const markdown_core_inline_ops *inlines = S_inlines_of(elements[i]);
        for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
            if (S_opens_implement(opens, (markdown_core_block_hook)hook)) {
                sizes->block_totals[hook]++;
                sizes->pointers++;
                if (S_opens_gate(opens, (markdown_core_block_hook)hook).bytes) {
                    sizes->gated[hook] = true;
                }
            }
        }
        for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
            if (S_inlines_implement(inlines, (markdown_core_inline_hook)hook)) {
                sizes->inline_totals[hook]++;
                sizes->pointers++;
            }
        }
        sizes->steps += S_count_finish_keys(elements[i], sizes->finish_key_counts);
        sizes->state_bytes += markdown_core_state_align(elements[i]->state_size);
        sizes->run_state_bytes += markdown_core_state_align(elements[i]->run_state_size);
        sizes->pointers += elements[i]->names != NULL;
        sizes->pointers += elements[i]->stages != NULL;
        for (const char *const *name = elements[i]->needs; name && *name; name++) {
            sizes->needs++;
        }
        for (const markdown_core_space *const *space = elements[i]->spaces; space && *space; space++) {
            sizes->namings++;
        }
        sizes->closers += inlines->close_count;
    }
    sizes->pointers += S_count_inline_dispatch(elements, count, sizes->inline_dispatch_offsets);
    sizes->pointers += 2; /* the name owners' and the staged instances' terminators */
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        sizes->steps += sizes->finish_key_counts[key] != 0; /* the terminator */
    }
    /* The instance table: a power of two at least twice the elements, so a
     * lookup always finds an empty entry to stop at, and at least two, so the
     * hash takes a bit. */
    for (sizes->instance_bits = 1; ((size_t)1 << sizes->instance_bits) < 2 * count; sizes->instance_bits++) {
    }
    /* A family with no declared gate keeps no table and every owner is asked. */
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        if (sizes->gated[hook]) {
            sizes->gate_bytes += MARKDOWN_CORE_BLOCK_GATE_KEYS * (sizes->block_totals[hook] + 1);
        }
    }
    sizes->spaces = S_count_spaces(elements, count, SIZE_MAX);
    return sizes->steps * sizeof(markdown_core_finish_step_entry) + count * sizeof(markdown_core_element_instance) +
           sizes->closers * sizeof(markdown_core_closer) +
           (sizes->pointers + ((size_t)1 << sizes->instance_bits)) * sizeof(const markdown_core_element_instance *) +
           sizes->needs * sizeof(markdown_core_need) + (sizes->spaces + 2) * sizeof(const markdown_core_space *) +
           sizes->namings * sizeof(unsigned) + sizes->gate_bytes;
}

/* THE RULES FOR CLOSING A BRACKET (element.h, markdown_core_close_rule):
 * every element's, by precedence and then descriptor order, a stable
 * insertion into the `count` entries at `closers`. */
/* The rules of the instance `i` whose `mine` is `mine`, put in precedence
 * order after the `at` rules `closers` holds: how many it holds then. */
static size_t S_place_closers(markdown_core_dialect *dialect, markdown_core_closer *closers, size_t at, size_t from,
                              size_t i, bool mine) {
    const markdown_core_inline_ops *inlines = S_inlines_of(dialect->instances[i].element);
    for (size_t r = 0; r < inlines->close_count; r++) {
        const markdown_core_close_rule *rule = &inlines->closes[r];
        if (rule->mine != mine) {
            continue;
        }
        size_t to = at++;
        while (to > from && closers[to - 1].rule->precedence > rule->precedence) {
            closers[to] = closers[to - 1];
            to--;
        }
        closers[to] = (markdown_core_closer){&dialect->instances[i], rule, inlines->closer};
    }
    return at;
}

static void S_project_closers(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                              markdown_core_closer *closers, size_t count) {
    size_t at = 0;
    for (size_t i = 0; i < dialect->element_count; i++) {
        at = S_place_closers(dialect, closers, at, 0, i, false);
    }
    dialect->shared = at;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const size_t first = at;
        at = S_place_closers(dialect, closers, at, first, i, true);
        instances[i].mine_first = (uint32_t)first;
        instances[i].mine_count = (uint32_t)(at - first);
    }
    assert(at == count);
    dialect->closers = closers;
    dialect->closer_count = count;
    /* The engine reads a closer whenever a rule may close a bracket at it,
     * so the byte ends a text run unconditionally. */
    for (size_t i = 0; i < count; i++) {
        const unsigned char byte = closers[i].byte;
        dialect->closes[byte] = true;
        dialect->special_chars[byte] = MARKDOWN_CORE_TEXT_END;
        dialect->inline_start_owners[byte] = NULL;
    }
}

/* SEAL: every table the dialect decides, projected once, into the storage
 * `sizes` was measured for. The tail after the struct holds the finish step
 * entries, then the instances, then the rules for closing a bracket, then the instance-pointer lists (the block
 * families, the inline-content families, the inline dispatch), the
 * instance table and the resolved needs, then the gate tables. Each region's alignment is at most
 * the one before it, so each starts where the one before ends. The element
 * list is copied into the instances rather than taken, so the dialect owns
 * nothing apart from its storage and the builder still owns what it did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder, const markdown_core_dialect_sizes *sizes,
                                markdown_core_dialect *dialect, unsigned char *state) {
    size_t count = builder->element_count;
    markdown_core_finish_step_entry *step_entries = (markdown_core_finish_step_entry *)(dialect + 1);
    markdown_core_element_instance *instances = (markdown_core_element_instance *)(step_entries + sizes->steps);
    markdown_core_closer *closers = (markdown_core_closer *)(instances + count);
    const markdown_core_element_instance **entries =
        (const markdown_core_element_instance **)(closers + sizes->closers);
    const markdown_core_element_instance **table = entries + sizes->pointers;
    markdown_core_need *resolved = (markdown_core_need *)(table + ((size_t)1 << sizes->instance_bits));
    const markdown_core_space **spaces = (const markdown_core_space **)(resolved + sizes->needs);
    unsigned *namings = (unsigned *)(spaces + sizes->spaces + 2);
    uint8_t *tables = (uint8_t *)(namings + sizes->namings);

    dialect->element_count = count;
    S_project_instances(dialect, builder->elements, instances, table, sizes->instance_bits, state);
    assert(dialect->run_state_size == sizes->run_state_bytes);
    S_resolve_needs(dialect, instances, resolved);
    memset(dialect->facets, 0, sizeof(dialect->facets));
    for (size_t i = 0; i < count; i++) {
        instances[i].dialect = dialect;
        for (const markdown_core_facet *facet = instances[i].element->facets;
             facet && facet->kind != MARKDOWN_CORE_KIND_NONE; facet++) {
            if ((size_t)facet->kind < MARKDOWN_CORE_FACET_ROOM) {
                dialect->facets[facet->kind] = facet;
            }
        }
    }
    S_number_spaces(dialect, instances, spaces, namings);
    S_resolve_owners(dialect);
    S_project_kinds(dialect);
    S_project_inline_bytes(dialect);
    /* What a container continuation may strip, as one table over the byte:
     * indentation, which every continuation strips, and the bytes each
     * container element declares for its own. */
    dialect->container_prefix[' '] = dialect->container_prefix['\t'] = true;
    for (size_t i = 0; i < count; i++) {
        const char *bytes = instances[i].lines->prefix_bytes;
        for (const unsigned char *c = (const unsigned char *)bytes; bytes && *c; c++) {
            dialect->container_prefix[*c] = true;
        }
        const markdown_core_open_ops *opens = S_opens_of(instances[i].element);
        const char *claims = opens->claims;
        for (const unsigned char *c = (const unsigned char *)claims; claims && *c; c++) {
            dialect->claims[*c] = true;
        }
        if (opens->cut) {
            const unsigned char *needs = (const unsigned char *)opens->cut_needs.bytes;
            if (!needs) {
                memset(dialect->fronts, 0xFF, sizeof(dialect->fronts));
            }
            for (const unsigned char *c = needs; needs && *c; c++) {
                dialect->fronts[*c >> 3] |= (uint8_t)(1u << (*c & 7));
            }
        }
    }

    size_t at = 0;
    for (size_t hook = 0; hook < MARKDOWN_CORE_BLOCK_HOOK_COUNT; hook++) {
        dialect->block_hooks[hook] = entries + at;
        dialect->block_hook_counts[hook] = sizes->block_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_opens_implement(S_opens_of(instances[i].element), (markdown_core_block_hook)hook)) {
                entries[at++] = &instances[i];
            }
        }
    }
    for (size_t hook = 0; hook < MARKDOWN_CORE_INLINE_HOOK_COUNT; hook++) {
        dialect->inline_hooks[hook] = entries + at;
        dialect->inline_hook_counts[hook] = sizes->inline_totals[hook];
        for (size_t i = 0; i < count; i++) {
            if (S_inlines_implement(S_inlines_of(instances[i].element), (markdown_core_inline_hook)hook)) {
                entries[at++] = &instances[i];
            }
        }
    }
    dialect->namers = entries + at;
    for (size_t i = 0; i < count; i++) {
        if (instances[i].element->names) {
            entries[at++] = &instances[i];
        }
    }
    entries[at++] = NULL;
    dialect->staged = entries + at;
    for (size_t i = 0; i < count; i++) {
        if (instances[i].element->stages) {
            entries[at++] = &instances[i];
        }
    }
    entries[at++] = NULL;
    memcpy(dialect->inline_dispatch_offsets, sizes->inline_dispatch_offsets, sizeof(dialect->inline_dispatch_offsets));
    S_project_inline_dispatch(dialect, entries + at);
    at += dialect->inline_dispatch_offsets[256];
    assert(at == sizes->pointers);

    S_project_closers(dialect, instances, closers, sizes->closers);
    S_project_finish_steps(dialect, sizes->finish_key_counts, step_entries);
    S_project_gate_lists(dialect, sizes, tables);
}
