#include "alloc.h"
#include "heading_scanners.h"
#include "citation.h"
#include "heading.h"
#include "link.h"
#include "directive.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { HEADING_CITATION };
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
/* A heading closes into the element's collection, which declares its label
 * and assigns its anchor as the parse ends. */
static bool close_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, markdown_core_node *parent) {
    (void)parent;
    markdown_core_heading_collection *headings = &((markdown_core_heading_state *)self->state)->headings;
    if (headings->count == headings->capacity) {
        size_t capacity = headings->capacity ? headings->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*headings->values)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return true;
        }
        void *values = markdown_core_realloc(headings->values, capacity * sizeof(*headings->values));
        if (!values) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return true;
        }
        headings->values = values;
        headings->capacity = capacity;
    }
    headings->values[headings->count++] = (markdown_core_heading_parse){.node = node, .start = node->where.place.start};
    return true;
}

static uint64_t heading_start_key(const void *entry) { return ((const markdown_core_heading_parse *)entry)->start; }

/* The inline content of every heading that settled since the last
 * completion: first, in source order, each one's inlines up to its
 * declaration dependency and the implicit reference it declares, then the
 * rest of their inlines. The headings before the collection's `parsed` come
 * before every later one in source order, so only the headings after them
 * are put in order: those that closed since, and those the last completion
 * left because they had not settled. A heading a streaming commit settled is
 * named by its path from then on: a publish exchanges its node with the
 * object it continues. */
void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    const size_t first = headings->parsed;
    if (first < headings->count &&
        !markdown_core_order_source_entries(&parser->source_order, &headings->values[first], headings->count - first,
                                            sizeof(*headings->values), heading_start_key)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    /* Anchors are numbered in source order, whatever order mapped inputs were
     * scheduled and headings closed in. */
    for (size_t i = first; i < headings->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = &headings->values[i];
        if (!heading->parsed && markdown_core_parser_settled(parser, heading->node)) {
            parser->completing = heading->node;
            markdown_core_prepare_heading(self, parser, heading);
        }
    }
    for (size_t i = first; i < headings->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = &headings->values[i];
        if (heading->parsed || !markdown_core_parser_settled(parser, heading->node)) {
            continue;
        }
        markdown_core_node *node = heading->node;
        parser->completing = node;
        markdown_core_finish_heading(parser, heading);
        heading->parsed = true;
        heading->authored = node->attributes.anchor.len != 0;
        if (parser->streaming && (node->flags & MARKDOWN_CORE_NODE__COMMITTED)) {
            heading->path = markdown_core_parser_path(parser, node);
            heading->node = NULL;
        }
    }
    while (headings->parsed < headings->count && headings->values[headings->parsed].parsed) {
        headings->parsed++;
    }
}

/* The parsed heading that starts at `start`, found by halves among those in
 * source order. */
static markdown_core_heading_parse *parsed_heading(markdown_core_heading_collection *headings, uint32_t start) {
    size_t low = 0, high = headings->parsed;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (headings->values[middle].start < start) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    assert(low < headings->parsed && headings->values[low].start == start);
    return &headings->values[low];
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
    markdown_core_heading_parse *entry =
        parsed_heading(&((markdown_core_heading_state *)self->state)->headings, node->where.place.start);
    entry->authored = authored;
    entry->name = NULL;
}

/* A SNAPSHOT'S HEADINGS (element.h, `start_snapshot`). After a commit every
 * heading is parsed but the pending block's, which a later line can still
 * write: it closed last, so it comes after every other heading in source
 * order. While the snapshot lasts the pending block's copy stands in for it,
 * and the entry names the copy. */
void markdown_core_headings_start_snapshot(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    state->snapshot_count = headings->count;
    state->snapshot_parsed = headings->parsed;
    assert(headings->count - headings->parsed <= 1);
    if (headings->parsed < headings->count) {
        assert(!headings->values[headings->parsed].parsed && parser->copy_count > parser->builder_count &&
               headings->values[headings->parsed].node == parser->copies[parser->builder_count].original);
        headings->values[headings->parsed].node = parser->pending;
    }
}

/* The snapshot ends with the collection as its commit left it: the entries
 * it made go, and the pending block's entry names the block again, not
 * parsed. The anchors it computed for the headings commits settled stay,
 * which the published document holds. */
void markdown_core_headings_end_snapshot(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    for (size_t i = state->snapshot_parsed; i < headings->count; i++) {
        markdown_core_dispose_heading(&headings->values[i]);
    }
    headings->count = headings->parsed = state->snapshot_parsed;
    if (state->snapshot_count > state->snapshot_parsed) {
        markdown_core_node *pending = parser->pending;
        headings->values[headings->count++] =
            (markdown_core_heading_parse){.node = pending, .start = pending->where.place.start};
    }
}

/* Release the headings. The anchors are assigned by then, or the parse
 * failed; either way nothing reads them again. */
void markdown_core_headings_dispose(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    for (size_t i = 0; i < headings->count; i++) {
        markdown_core_dispose_heading(&headings->values[i]);
    }
    markdown_core_free(headings->values);
    *headings = (markdown_core_heading_collection){0};
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

/* The entry for `name` among the names taken, occupied or vacant, or NULL
 * when the index could not grow. */
static markdown_core_key_index_slot *taken_entry(markdown_core_heading_state *state, markdown_core_key_index *taken,
                                                 const unsigned char *name, bufsize_t length) {
    state->anchor_work += (size_t)length + 1;
    return markdown_core_key_index_entry(taken, name, length);
}

/* The heading's node in the tree the parser builds: the one its entry
 * names, or the block at its path. NULL, with the parse failed, when the path
 * could not be followed. */
static markdown_core_node *heading_node(markdown_core_parser *parser, const markdown_core_heading_parse *heading) {
    return heading->node ? heading->node : markdown_core_parser_resolve(parser, heading->path);
}

/* `name`, `size` bytes of which the first `base_size` are the base, given to
 * the heading as its computed anchor, unless it holds it: a heading at a path
 * is first an object of its own (markdown_core_parser_supersede), and the
 * anchor it held goes after the next publish when the published document
 * reads it, and at once otherwise. The node's bytes, or NULL with the parse
 * failed. */
static const unsigned char *heading_name(markdown_core_parser *parser, markdown_core_heading_parse *heading,
                                         markdown_core_node *node, const unsigned char *name, bufsize_t size,
                                         bufsize_t base_size) {
    markdown_core_chunk *anchor = &node->attributes.anchor;
    if (!(node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) || anchor->len != size ||
        memcmp(anchor->data, name, (size_t)size) != 0) {
        markdown_core_chunk made = {(unsigned char *)name, size, 0};
        if (!markdown_core_chunk_to_cstr(&made)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        markdown_core_node *const held = node;
        if (heading->path && !(node = markdown_core_parser_supersede(parser, heading->path))) {
            markdown_core_chunk_free(&made);
            return NULL;
        }
        anchor = &node->attributes.anchor;
        if (!(node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED)) {
            *anchor = made;
        } else if (node == held) {
            markdown_core_chunk_free(anchor);
            *anchor = made;
        } else if (markdown_core_parser_retire(parser, anchor->data, MARKDOWN_CORE_RETIRED_CONTENT)) {
            *anchor = made;
        } else {
            markdown_core_chunk_free(&made);
            return NULL;
        }
        node->flags |= MARKDOWN_CORE_NODE__ANCHOR_COMPUTED;
    }
    heading->name = anchor->data;
    heading->name_size = size;
    heading->base_size = base_size;
    return anchor->data;
}

/* Each heading's anchor, in source order: an authored one stays, and a
 * computed one is the heading's own copy of the first name its text projects
 * to that no explicit anchor reserves and no heading before it was given. The
 * names taken are this pass's own index: first every explicit anchor, the
 * registry's ANCHOR records under their own bytes in log order, a name
 * leaving the index again at a withdrawal that leaves no declaration of it
 * standing; then each name a heading is given. A taken name's entry also
 * keeps the suffix to try next, so a run of headings projecting to one name
 * tries each suffix once, and each candidate costs one entry lookup, which a
 * vacant name the heading is given commits in place. A heading whose entry
 * keeps the anchor it was computed takes its base from it, and keeps it when
 * it is the name chosen: only a heading whose anchor changes is written. */
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    /* A pass whose headings all have authored anchors chooses no name. */
    size_t first = 0;
    while (first < headings->count && headings->values[first].authored) {
        first++;
    }
    if (first == headings->count) {
        return;
    }
    const markdown_core_registry *registry = &parser->registry;
    markdown_core_registry_cursor anchors = markdown_core_registry_records(registry, MARKDOWN_CORE_REGISTRY_ANCHOR);
    markdown_core_key_index taken;
    if (!markdown_core_key_index_init(&taken, headings->count + anchors.left)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    for (const markdown_core_registry_record *reserved; (reserved = markdown_core_registry_next(registry, &anchors));) {
        const markdown_core_chunk name = markdown_core_registry_label(reserved);
        if (reserved->withdraws) {
            /* A declaration of the name came before, which entered it. */
            if (!reserved->live) {
                state->anchor_work += (size_t)name.len + 1;
                markdown_core_key_index_remove(&taken, markdown_core_key_index_find(&taken, name.data, name.len));
            }
            continue;
        }
        markdown_core_key_index_slot *entry = taken_entry(state, &taken, name.data, name.len);
        if (!entry) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            break;
        }
        if (!entry->key) {
            markdown_core_key_index_commit(&taken, entry, name.data);
            entry->value = 1;
        }
    }
    markdown_core_strbuf base = MARKDOWN_CORE_BUF_INIT();
    anchor_projection_stack stack = {0};
    for (size_t i = first; i < headings->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = &headings->values[i];
        if (heading->authored) {
            continue;
        }
        markdown_core_node *node = NULL;
        markdown_core_strbuf_clear(&base);
        if (heading->name) {
            state->anchor_work += (size_t)heading->base_size;
            markdown_core_strbuf_put(&base, heading->name, heading->base_size);
            if (base.oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
        } else if ((node = heading_node(parser, heading))) {
            heading_anchor_base(parser, state, node, &base, &stack);
        }
        if (parser->error) {
            break;
        }
        const bufsize_t base_size = base.size;
        /* The projected name's entry, which keeps its next suffix. Only a
         * vacant candidate can grow the index, and the suffix is advanced
         * before each candidate's lookup, so the entry is never read after
         * a lookup that could have moved it. */
        markdown_core_key_index_slot *projected = taken_entry(state, &taken, base.ptr, base.size);
        markdown_core_key_index_slot *candidate = projected;
        while (candidate && candidate->key) {
            markdown_core_strbuf_truncate(&base, base_size);
            append_anchor_suffix(&base, projected->value++);
            if (base.oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
            candidate = taken_entry(state, &taken, base.ptr, base.size);
        }
        if (!candidate) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        if (parser->error) {
            break;
        }
        const unsigned char *name = heading->name;
        if (!name || heading->name_size != base.size || memcmp(name, base.ptr, (size_t)base.size) != 0) {
            name = (node || (node = heading_node(parser, heading)))
                       ? heading_name(parser, heading, node, base.ptr, base.size, base_size)
                       : NULL;
            if (!name) {
                break;
            }
        }
        markdown_core_key_index_commit(&taken, candidate, name);
        candidate->value = 1;
    }
    markdown_core_key_index_free(&taken);
    markdown_core_free(stack.values);
    markdown_core_strbuf_free(&base);
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
            if (!markdown_core_registry_normal_label(&parser->registry, parser->pool, &label, declared) ||
                !markdown_core_registry_declare(&parser->registry, MARKDOWN_CORE_REGISTRY_REFERENCE, declared->data,
                                                declared->len, heading->node->where.place.start)) {
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

#define peek_at(input, at) ((input)->data[(at)])
static bool open_atx(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    bufsize_t hashpos;
    int level = 0;
    bufsize_t heading_startpos = parser->first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
    if (!*container) {
        return false;
    }

    hashpos = markdown_core_chunk_strchr(input, '#', parser->first_nonspace);

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
        markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    }

    return true;
}
static bool scan_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         block_start_context *context, block_start *start) {
    (void)self;
    if ((start->matched = scan_atx_heading_start(context->input->data, context->input->len, context->first))) {
        start->open = open_atx;
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
    .run_state_size = sizeof(markdown_core_heading_run),
};
