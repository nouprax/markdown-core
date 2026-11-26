#include <assert.h>
#include <string.h>

#include "alloc.h"
#include "block_internal.h"
#include "dialect.h"
#include "label.h"
#include "labels.h"

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
    return document && document->init && document->read && document->publish && document->reopen && document->splice &&
           MARKDOWN_CORE_NODE_TYPE_BLOCK_P(document->kind) && S_finish_kind_indexable(document->kind);
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
    if (inlines->data &&
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

static bool S_inlines_implement(const markdown_core_inline_ops *inlines, markdown_core_inline_hook hook) {
    switch (hook) {
    case MARKDOWN_CORE_INLINE_HOOK_INIT:
        return inlines->init != NULL;
    case MARKDOWN_CORE_INLINE_HOOK_DISPOSE:
        return inlines->dispose != NULL;
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
        if (S_inlines_of(element)->data) {
            dialect->text_structure = instance;
        }
        if (S_opens_of(element)->content) {
            dialect->text_block_structure = instance;
        }
        if (element->spelling) {
            dialect->spelling = element->spelling;
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
    }
    /* The out-of-table index answers nothing; the storage is zeroed. */
}

/* THE FLANKING-TRANSPARENT BYTES, which flanking looks through when an
 * element declares them (`transparent`), a property of the registered
 * elements alone, projected once as the dialect is sealed. */
static void S_project_transparent(markdown_core_dialect *dialect) {
    for (size_t i = 0; i < dialect->element_count; i++) {
        const char *bytes = S_inlines_of(dialect->instances[i].element)->transparent;
        for (const unsigned char *c = (const unsigned char *)bytes; bytes && *c; c++) {
            dialect->skip_chars[*c] = 1;
        }
    }
}

/* Whether byte `*at` of `bytes` is written earlier in it: a byte written
 * twice in one declaration is one declaration. */
static bool S_declared_earlier(const unsigned char *bytes, const unsigned char *at) {
    for (const unsigned char *p = bytes; p < at; p++) {
        if (*p == *at) {
            return true;
        }
    }
    return false;
}

/* THE TEXT (plan symbols-cleanroom.md 23): the elements' constructs, the
 * entries by byte, which `offsets` counts as running totals, and the token
 * kinds, counted into `sizes`. */
static void S_count_text(const markdown_core_element *const *elements, size_t count, markdown_core_dialect_sizes *sizes,
                         uint16_t *offsets) {
    size_t at[257] = {0};
    sizes->token_kinds = MARKDOWN_CORE_TOKEN_FIRST;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_inline_ops *inlines = S_inlines_of(elements[i]);
        sizes->constructs += inlines->construct_count;
        sizes->token_kinds += inlines->token_count + S_opens_of(elements[i])->token_count;
        for (size_t k = 0; k < inlines->construct_count; k++) {
            const unsigned char *bytes = (const unsigned char *)inlines->constructs[k].bytes;
            for (const unsigned char *c = bytes; c && *c; c++) {
                if (!S_declared_earlier(bytes, c)) {
                    at[*c + 1]++;
                }
            }
        }
    }
    /* A byte a rule closes a label at reads the label's end, after every
     * element's construct there (label.h). */
    bool ends[256] = {false};
    for (size_t i = 0; i < count; i++) {
        const markdown_core_inline_ops *inlines = S_inlines_of(elements[i]);
        if (inlines->close_count && !ends[inlines->closer]) {
            ends[inlines->closer] = true;
            at[inlines->closer + 1]++;
        }
    }
    for (size_t c = 0; c < 256; c++) {
        at[c + 1] += at[c];
    }
    sizes->text_entries = at[256];
    for (size_t c = 0; c <= 256; c++) {
        offsets[c] = (uint16_t)at[c];
    }
}

/* The constructs copied into `copies` with their instances as context and
 * listed by byte into `items` -- by ascending precedence of their
 * elements, then descriptor order, then each element's own order -- and
 * the handlers copied into `handlers` and listed by kind into `compiles`,
 * the data's last, past the elements' kinds. */
static void S_project_text(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                           markdown_core_construct *copies, const markdown_core_construct **items,
                           const markdown_core_construct **holders, markdown_core_compile_ops *handlers,
                           const markdown_core_compile_ops **compiles) {
    const markdown_core_element_instance *owners[MARKDOWN_CORE_ELEMENT_LIMIT];
    size_t count = 0;
    uint16_t kind = MARKDOWN_CORE_TOKEN_FIRST;
    size_t data_slot = 0;
    for (size_t i = 0; i < dialect->element_count; i++) {
        data_slot += S_inlines_of(instances[i].element)->token_count + S_opens_of(instances[i].element)->token_count;
    }
    for (size_t k = 0; k < MARKDOWN_CORE_TOKEN_FIRST; k++) {
        compiles[k] = NULL;
    }
    for (size_t i = 0; i < dialect->element_count; i++) {
        markdown_core_element_instance *instance = &instances[i];
        const markdown_core_inline_ops *inlines = S_inlines_of(instance->element);
        instance->token_base = kind;
        if (inlines->data) {
            markdown_core_compile_ops *data = &handlers[data_slot];
            *data = *inlines->data;
            data->context = instance;
            compiles[MARKDOWN_CORE_TOKEN_DATA] = data;
        }
        for (size_t t = 0; t < inlines->token_count; t++, kind++) {
            handlers[kind - MARKDOWN_CORE_TOKEN_FIRST] = inlines->tokens[t];
            handlers[kind - MARKDOWN_CORE_TOKEN_FIRST].context = instance;
            compiles[kind] = &handlers[kind - MARKDOWN_CORE_TOKEN_FIRST];
        }
        const markdown_core_open_ops *opens = S_opens_of(instance->element);
        instance->block_base = kind;
        for (size_t t = 0; t < opens->token_count; t++, kind++) {
            handlers[kind - MARKDOWN_CORE_TOKEN_FIRST] = opens->tokens[t];
            handlers[kind - MARKDOWN_CORE_TOKEN_FIRST].context = instance;
            compiles[kind] = &handlers[kind - MARKDOWN_CORE_TOKEN_FIRST];
        }
        if (!inlines->construct_count) {
            continue;
        }
        size_t at = count++;
        while (at > 0 && S_inlines_of(owners[at - 1]->element)->precedence > inlines->precedence) {
            owners[at] = owners[at - 1];
            at--;
        }
        owners[at] = instance;
    }
    dialect->token_kinds = kind;
    dialect->compiles = compiles;
    size_t next[256], copied = 0;
    for (size_t c = 0; c < 256; c++) {
        next[c] = dialect->text_offsets[c];
    }
    for (size_t i = 0; i < count; i++) {
        const markdown_core_inline_ops *inlines = S_inlines_of(owners[i]->element);
        for (size_t k = 0; k < inlines->construct_count; k++) {
            markdown_core_construct *copy = &copies[copied++];
            *copy = inlines->constructs[k].construct;
            copy->context = owners[i];
            const unsigned char *bytes = (const unsigned char *)inlines->constructs[k].bytes;
            for (const unsigned char *c = bytes; c && *c; c++) {
                if (!S_declared_earlier(bytes, c)) {
                    items[next[*c]++] = copy;
                }
            }
        }
    }
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_inline_ops *inlines = S_inlines_of(instances[i].element);
        const unsigned char c = inlines->closer;
        if (inlines->close_count && next[c] < dialect->text_offsets[c + 1] &&
            (next[c] == dialect->text_offsets[c] || items[next[c] - 1] != &MARKDOWN_CORE_LABEL_END)) {
            items[next[c]++] = &MARKDOWN_CORE_LABEL_END;
        }
    }
    for (size_t k = MARKDOWN_CORE_TOKEN_LABEL; k <= MARKDOWN_CORE_TOKEN_MARKER; k++) {
        compiles[k] = &MARKDOWN_CORE_LABEL_TOKENS[k - MARKDOWN_CORE_TOKEN_LABEL];
    }
    /* The constructs that may hold something open, asked at every start
     * of the text (markdown_core_tokenizer_settled). */
    size_t held = 0;
    for (size_t i = 0; i < copied; i++) {
        if (copies[i].holds) {
            holders[held++] = &copies[i];
        }
    }
    dialect->text.items = items;
    dialect->text.offsets = dialect->text_offsets;
    dialect->text.holders = holders;
    dialect->text.holder_count = held;
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
    size_t kinds = MARKDOWN_CORE_LABEL_ANCHOR + 1;
    spaces[0] = spaces[MARKDOWN_CORE_LABEL_ANCHOR] = NULL;
    for (size_t i = 0; i < dialect->element_count; i++) {
        instances[i].spaces = namings;
        for (const markdown_core_space *const *space = instances[i].element->spaces; space && *space; space++) {
            size_t kind = MARKDOWN_CORE_LABEL_ANCHOR + 1;
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

/* THE STARTS (element.h, markdown_core_block_construct): the keys a start
 * is listed under, `keys` of them at most, written into `into`: every first
 * byte it declares, once, or every byte and the blank line when it
 * declares none, and the indented line when its indent reaches one. */
static size_t S_start_keys(const markdown_core_block_construct *start, uint16_t *into) {
    size_t count = 0;
    if (!start->bytes) {
        for (size_t key = 0; key <= MARKDOWN_CORE_BLOCK_GATE_KEY_NONE; key++) {
            into[count++] = (uint16_t)key;
        }
    } else {
        const unsigned char *bytes = (const unsigned char *)start->bytes;
        for (const unsigned char *c = bytes; *c; c++) {
            if (!S_declared_earlier(bytes, c)) {
                into[count++] = *c;
            }
        }
    }
    if (start->indent >= CODE_INDENT) {
        into[count++] = MARKDOWN_CORE_BLOCK_GATE_KEY_INDENTED;
    }
    return count;
}

/* The starts' entries by key, counted into `offsets` as running totals. */
static size_t S_count_starts(const markdown_core_element *const *elements, size_t count, size_t *starts,
                             uint16_t *offsets) {
    size_t at[MARKDOWN_CORE_BLOCK_GATE_KEYS + 1] = {0};
    uint16_t keys[MARKDOWN_CORE_BLOCK_GATE_KEYS];
    *starts = 0;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_open_ops *opens = S_opens_of(elements[i]);
        *starts += opens->start_count;
        for (size_t k = 0; k < opens->start_count; k++) {
            size_t listed = S_start_keys(&opens->starts[k], keys);
            for (size_t j = 0; j < listed; j++) {
                at[keys[j] + 1]++;
            }
        }
    }
    for (size_t key = 0; key < MARKDOWN_CORE_BLOCK_GATE_KEYS; key++) {
        at[key + 1] += at[key];
    }
    for (size_t key = 0; key <= MARKDOWN_CORE_BLOCK_GATE_KEYS; key++) {
        offsets[key] = (uint16_t)at[key];
    }
    return at[MARKDOWN_CORE_BLOCK_GATE_KEYS];
}

/* The starts copied into `copies` with their instances as context, and
 * listed by key into `items` in ascending precedence, then descriptor
 * order, then each element's own order. */
static void S_project_starts(markdown_core_dialect *dialect, markdown_core_element_instance *instances,
                             markdown_core_block_construct *copies, const markdown_core_block_construct **items,
                             size_t total) {
    size_t copied = 0;
    for (size_t i = 0; i < dialect->element_count; i++) {
        const markdown_core_open_ops *opens = S_opens_of(instances[i].element);
        for (size_t k = 0; k < opens->start_count; k++) {
            markdown_core_block_construct copy = opens->starts[k];
            copy.construct.context = &instances[i];
            /* A stable insertion by precedence. */
            size_t at = copied++;
            while (at > 0 && copies[at - 1].precedence > copy.precedence) {
                copies[at] = copies[at - 1];
                at--;
            }
            copies[at] = copy;
        }
    }
    assert(copied == total);
    size_t next[MARKDOWN_CORE_BLOCK_GATE_KEYS];
    uint16_t keys[MARKDOWN_CORE_BLOCK_GATE_KEYS];
    for (size_t key = 0; key < MARKDOWN_CORE_BLOCK_GATE_KEYS; key++) {
        next[key] = dialect->start_offsets[key];
    }
    for (size_t i = 0; i < copied; i++) {
        size_t listed = S_start_keys(&copies[i], keys);
        for (size_t j = 0; j < listed; j++) {
            items[next[keys[j]]++] = &copies[i];
        }
    }
    dialect->starts = items;
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
        if (opens->item) {
            sizes->cutters++;
            sizes->pointers++;
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
    uint16_t text_offsets[257];
    S_count_text(elements, count, sizes, text_offsets);
    uint16_t start_offsets[MARKDOWN_CORE_BLOCK_GATE_KEYS + 1];
    sizes->start_entries = S_count_starts(elements, count, &sizes->starts, start_offsets);
    sizes->pointers += 2; /* the name owners' and the staged instances' terminators */
    for (size_t key = 0; key < MARKDOWN_CORE_FINISH_KEY_COUNT; key++) {
        sizes->steps += sizes->finish_key_counts[key] != 0; /* the terminator */
    }
    /* The instance table: a power of two at least twice the elements, so a
     * lookup always finds an empty entry to stop at, and at least two, so the
     * hash takes a bit. */
    for (sizes->instance_bits = 1; ((size_t)1 << sizes->instance_bits) < 2 * count; sizes->instance_bits++) {
    }
    sizes->spaces = S_count_spaces(elements, count, SIZE_MAX);
    return sizes->steps * sizeof(markdown_core_finish_step_entry) + count * sizeof(markdown_core_element_instance) +
           sizes->closers * sizeof(markdown_core_closer) + sizes->constructs * sizeof(markdown_core_construct) +
           sizes->starts * sizeof(markdown_core_block_construct) +
           (sizes->token_kinds - MARKDOWN_CORE_TOKEN_FIRST + 1) * sizeof(markdown_core_compile_ops) +
           (sizes->text_entries + sizes->constructs + sizes->start_entries + sizes->token_kinds) * sizeof(void *) +
           (sizes->pointers + ((size_t)1 << sizes->instance_bits)) * sizeof(const markdown_core_element_instance *) +
           sizes->needs * sizeof(markdown_core_need) + (sizes->spaces + 2) * sizeof(const markdown_core_space *) +
           sizes->namings * sizeof(unsigned);
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
}

/* SEAL: every table the dialect decides, projected once, into the storage
 * `sizes` was measured for. The tail after the struct holds the finish step
 * entries, then the instances, then the rules for closing a label, the
 * constructs and the starts, the handlers, the lists of constructs and of
 * starts, then the instance-pointer lists (the cutters and the
 * inline-content families), the instance table and the resolved needs. Each region's alignment is at most
 * the one before it, so each starts where the one before ends. The element
 * list is copied into the instances rather than taken, so the dialect owns
 * nothing apart from its storage and the builder still owns what it did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder, const markdown_core_dialect_sizes *sizes,
                                markdown_core_dialect *dialect, unsigned char *state) {
    size_t count = builder->element_count;
    markdown_core_finish_step_entry *step_entries = (markdown_core_finish_step_entry *)(dialect + 1);
    markdown_core_element_instance *instances = (markdown_core_element_instance *)(step_entries + sizes->steps);
    markdown_core_closer *closers = (markdown_core_closer *)(instances + count);
    markdown_core_construct *copies = (markdown_core_construct *)(closers + sizes->closers);
    markdown_core_block_construct *start_copies = (markdown_core_block_construct *)(copies + sizes->constructs);
    markdown_core_compile_ops *handlers = (markdown_core_compile_ops *)(start_copies + sizes->starts);
    const markdown_core_construct **items =
        (const markdown_core_construct **)(handlers + (sizes->token_kinds - MARKDOWN_CORE_TOKEN_FIRST + 1));
    const markdown_core_construct **holders = items + sizes->text_entries;
    const markdown_core_block_construct **start_items =
        (const markdown_core_block_construct **)(holders + sizes->constructs);
    const markdown_core_compile_ops **compiles =
        (const markdown_core_compile_ops **)(start_items + sizes->start_entries);
    const markdown_core_element_instance **entries =
        (const markdown_core_element_instance **)(compiles + sizes->token_kinds);
    const markdown_core_element_instance **table = entries + sizes->pointers;
    markdown_core_need *resolved = (markdown_core_need *)(table + ((size_t)1 << sizes->instance_bits));
    const markdown_core_space **spaces = (const markdown_core_space **)(resolved + sizes->needs);
    unsigned *namings = (unsigned *)(spaces + sizes->spaces + 2);

    dialect->element_count = count;
    S_project_instances(dialect, builder->elements, instances, table, sizes->instance_bits, state);
    assert(dialect->run_state_size == sizes->run_state_bytes);
    S_resolve_needs(dialect, instances, resolved);
    for (size_t k = 0; k < MARKDOWN_CORE_FACET_ROOM; k++) {
        dialect->facets[k] = NULL;
    }
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
    S_project_transparent(dialect);
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
        if (opens->item) {
            const unsigned char *needs = (const unsigned char *)opens->item->bytes;
            if (!needs) {
                memset(dialect->fronts, 0xFF, sizeof(dialect->fronts));
            }
            for (const unsigned char *c = needs; needs && *c; c++) {
                dialect->fronts[*c >> 3] |= (uint8_t)(1u << (*c & 7));
            }
        }
    }

    size_t at = 0;
    dialect->cutters = entries + at;
    dialect->cutter_count = sizes->cutters;
    for (size_t i = 0; i < count; i++) {
        if (S_opens_of(instances[i].element)->item) {
            entries[at++] = &instances[i];
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
    assert(at == sizes->pointers);

    S_project_closers(dialect, instances, closers, sizes->closers);
    S_count_text(builder->elements, count, &(markdown_core_dialect_sizes){0}, dialect->text_offsets);
    S_project_text(dialect, instances, copies, items, holders, handlers, compiles);
    S_project_finish_steps(dialect, sizes->finish_key_counts, step_entries);
    size_t starts = 0;
    S_count_starts(builder->elements, count, &starts, dialect->start_offsets);
    S_project_starts(dialect, instances, start_copies, start_items, starts);
}
