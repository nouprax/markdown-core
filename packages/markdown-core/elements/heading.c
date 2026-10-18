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
    headings->values[headings->count++] = (markdown_core_heading_parse){.node = node, .start = node->place.start};
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
        if (parser->stream && (node->flags & MARKDOWN_CORE_NODE__COMMITTED)) {
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
        parsed_heading(&((markdown_core_heading_state *)self->state)->headings, node->place.start);
    entry->authored = authored;
    if (entry->holds_name) {
        markdown_core_free((void *)entry->name);
        entry->holds_name = false;
    }
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
    state->snapshot_records = parser->registry.count;
    assert(headings->count - headings->parsed <= 1);
    if (headings->parsed < headings->count) {
        assert(
            !headings->values[headings->parsed].parsed && parser->stream->copy_count > parser->stream->builder_count &&
            headings->values[headings->parsed].node == parser->stream->copies[parser->stream->builder_count].original);
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
            (markdown_core_heading_parse){.node = pending, .start = pending->place.start};
    }
}

/* Release the headings and the names they took, with the committed names
 * their entries hold. The anchors are assigned by then, or the parse failed;
 * either way nothing reads them again. */
void markdown_core_headings_dispose(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    for (size_t i = 0; i < headings->count; i++) {
        markdown_core_dispose_heading(&headings->values[i]);
        if (headings->values[i].holds_name) {
            markdown_core_free((void *)headings->values[i].name);
        }
    }
    markdown_core_free(headings->values);
    *headings = (markdown_core_heading_collection){0};
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
 * key is the taker's bytes: the label of the registry record that reserves
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
 * of the registry's ANCHOR records stands and how many headings it named.
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

/* The heading's node in the tree the parser builds: the one its entry
 * names, or the block at its path. NULL, with the parse failed, when the path
 * could not be followed. */
static inline markdown_core_node *heading_node(markdown_core_parser *parser,
                                               const markdown_core_heading_parse *heading) {
    return heading->node ? heading->node : markdown_core_parser_resolve(parser, heading->path);
}

/* `made`, bytes the call takes when it succeeds, given to the heading's node
 * as its computed anchor: a heading at a path is first an object of its own
 * (markdown_core_parser_supersede). The anchor the node computed before goes
 * to `*kept` when `kept` is not NULL, and otherwise after the next publish
 * when the published document reads it, and at once when it does not. The
 * node's bytes, or NULL with the parse failed. */
static inline const unsigned char *show_anchor(markdown_core_parser *parser, const markdown_core_heading_parse *heading,
                                               markdown_core_node *node, markdown_core_chunk made,
                                               unsigned char **kept) {
    markdown_core_node *const held = node;
    if (heading->path && !(node = markdown_core_parser_supersede(parser, heading->path))) {
        return NULL;
    }
    markdown_core_chunk *anchor = &node->attributes.anchor;
    if (node->flags & MARKDOWN_CORE_NODE__ANCHOR_COMPUTED) {
        if (kept) {
            *kept = anchor->data;
        } else if (node == held) {
            markdown_core_chunk_free(anchor);
        } else if (!markdown_core_parser_retire(parser, anchor->data, MARKDOWN_CORE_RETIRED_CONTENT)) {
            return NULL;
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
    markdown_core_heading_parse *heading = &state->headings.values[at];
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

/* The heading's node, `node` when not NULL, shows the name in the pass's
 * buffer unless `shown`, `shown_size` bytes, which it shows, is that name;
 * the bytes it showed go as show_anchor says. The node's bytes, or NULL with
 * the parse failed. */
static inline const unsigned char *show_name(name_pass *pass, const markdown_core_heading_parse *heading,
                                             markdown_core_node *node, const unsigned char *shown, bufsize_t shown_size,
                                             unsigned char **kept) {
    markdown_core_parser *parser = pass->parser;
    const markdown_core_strbuf *name = &pass->name;
    if (shown && shown_size == name->size && memcmp(shown, name->ptr, (size_t)name->size) == 0) {
        return shown;
    }
    if (!node && !(node = heading_node(parser, heading))) {
        return NULL;
    }
    markdown_core_chunk made = {name->ptr, name->size, 0};
    if (!markdown_core_chunk_to_cstr(&made)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    if (!(shown = show_anchor(parser, heading, node, made, kept))) {
        markdown_core_chunk_free(&made);
    }
    return shown;
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
    markdown_core_heading_parse *heading = &state->headings.values[at];
    const unsigned char *shown;
    if (heading->holds_name) {
        markdown_core_node *node = heading_node(parser, heading);
        if (!node || !(shown = show_name(pass, heading, node, node->attributes.anchor.data, node->attributes.anchor.len,
                                         NULL))) {
            return NULL;
        }
    } else {
        unsigned char *kept = NULL;
        if (!(shown = show_name(pass, heading, NULL, heading->name, heading->name_size, &kept))) {
            return NULL;
        }
        assert(!kept || kept == heading->name);
        heading->holds_name = kept != NULL;
    }
    if (pass->beneath && at < state->snapshot_parsed) {
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
static inline const unsigned char *give_name(name_pass *pass, markdown_core_heading_parse *heading,
                                             bufsize_t base_size) {
    const unsigned char *shown = show_name(pass, heading, NULL, heading->name, heading->name_size, NULL);
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
    assert(!heading->holds_name && !(pass->beneath && at < pass->state->snapshot_parsed));
    const unsigned char *bytes = give_name(pass, heading, base_size);
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
        markdown_core_heading_parse *heading = &state->headings.values[at];
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
        const unsigned char *bytes = heading->holds_name || (pass->beneath && at < state->snapshot_parsed)
                                         ? give_name_apart(pass, at, base_size)
                                         : give_name(pass, heading, base_size);
        holder = bytes ? claim_name(pass, at, candidate, bytes) : 0;
    }
}

/* The record's name reserved: a vacant name becomes the record's, and one a
 * heading holds is taken from it, which goes on along its sequence. A
 * withdrawal that leaves no declaration of its name standing, which only a
 * pass that starts empty reads, before any heading takes a name, frees the
 * name: a declaration of it came before, which entered it. */
static void read_record(name_pass *pass, const markdown_core_registry_record *record) {
    markdown_core_parser *parser = pass->parser;
    const markdown_core_chunk label = markdown_core_registry_label(record);
    markdown_core_key_index *index = pass->index;
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
static void read_records(name_pass *pass, markdown_core_registry_cursor *cursor, size_t end) {
    const markdown_core_registry *registry = &pass->parser->registry;
    for (const markdown_core_registry_record *record;
         !pass->parser->error && (record = markdown_core_registry_next_before(registry, cursor, end));) {
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
        markdown_core_heading_parse *heading = &state->headings.values[at];
        if (heading->authored) {
            continue;
        }
        markdown_core_strbuf_clear(name);
        markdown_core_node *node;
        if (heading->name) {
            state->anchor_work += (size_t)heading->base_size;
            markdown_core_strbuf_put(name, heading->name, heading->base_size);
            if (name->oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
        } else if ((node = heading_node(parser, heading))) {
            heading_anchor_base(parser, state, node, name, &pass->stack);
        }
        if (!parser->error) {
            take_last(pass, heading, at, name->size);
        }
    }
}

/* A committed heading the last snapshot renamed shows its committed name
 * again: the entry's bytes go back to its node. */
static void restore_name(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    markdown_core_node *node = heading_node(parser, heading);
    const markdown_core_chunk name = {(unsigned char *)heading->name, heading->name_size, 1};
    if (node && show_anchor(parser, heading, node, name, NULL)) {
        heading->holds_name = false;
    }
}

/* A SNAPSHOT'S NAMES. The snapshot's tail -- the ANCHOR records its
 * completion made, then the headings it parsed, the pending block's copy
 * among them -- takes its names in an overlay over the committed names, which
 * stay as the commits left them. A reservation can displace committed
 * headings, which show the names the overlay gives them (A COMMITTED HEADING
 * UNDER ANOTHER NAME); then each committed heading the last snapshot renamed
 * and this one did not shows its committed name again, and the ones this one
 * renamed are the list. */
static void name_snapshot(name_pass *pass, markdown_core_registry_cursor cursor) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_collection *headings = &state->headings;
    markdown_core_key_index overlay;
    if (!markdown_core_key_index_init(&overlay, cursor.left + headings->count - state->snapshot_parsed)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    pass->index = &overlay;
    pass->beneath = &state->taken;
    const size_t earlier = state->renamed_count;
    read_records(pass, &cursor, parser->registry.count);
    name_headings(pass, state->snapshot_parsed, headings->count);
    for (size_t i = 0; i < earlier && !parser->error; i++) {
        markdown_core_heading_parse *heading = &headings->values[state->renamed[i]];
        if (heading->holds_name && !heading->renamed) {
            restore_name(parser, heading);
        }
    }
    size_t count = 0;
    for (size_t i = earlier; i < state->renamed_count; i++) {
        headings->values[state->renamed[i]].renamed = false;
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
static void names_reset(markdown_core_parser *parser, markdown_core_heading_state *state) {
    for (size_t i = 0; i < state->renamed_count; i++) {
        markdown_core_heading_parse *heading = &state->headings.values[state->renamed[i]];
        if (heading->holds_name) {
            markdown_core_node *node = heading_node(parser, heading);
            markdown_core_free((void *)heading->name);
            heading->holds_name = false;
            heading->name = node ? node->attributes.anchor.data : NULL;
            heading->name_size = node ? node->attributes.anchor.len : 0;
        }
    }
    state->renamed_count = 0;
    markdown_core_key_index_free(&state->taken);
    state->records_at = state->records_read = state->named = 0;
}

/* Each heading's anchor (THE COMMITTED NAMES): an authored one stays, and a
 * computed one is the heading's own copy of the name chosen; only a heading
 * whose anchor changes is written. A snapshot moves the committed names on,
 * by the records and the headings its commit added, and names its tail over
 * them; the finalize names everything from an empty index. */
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_collection *headings = &state->headings;
    const bool snapshot = parser->snapshot;
    markdown_core_parser_path_pass(parser);
    if (!snapshot) {
        names_reset(parser, state);
    }
    const size_t committed = snapshot ? state->snapshot_parsed : headings->count;
    if (!state->taken.size) {
        /* Nothing is named yet: the authored headings ask for no name. */
        while (state->named < committed && headings->values[state->named].authored) {
            state->named++;
        }
        size_t first = state->named;
        while (first < headings->count && headings->values[first].authored) {
            first++;
        }
        if (first == headings->count) {
            return;
        }
    }
    const markdown_core_registry *registry = &parser->registry;
    const size_t records = registry->spaces[MARKDOWN_CORE_REGISTRY_ANCHOR].records;
    markdown_core_registry_cursor cursor = {MARKDOWN_CORE_REGISTRY_ANCHOR, state->records_at,
                                            records - state->records_read};
    if (!state->taken.capacity && !markdown_core_key_index_init(&state->taken, headings->count + cursor.left)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    name_pass pass;
    pass.parser = parser;
    pass.state = state;
    pass.index = &state->taken;
    pass.beneath = NULL;
    pass.name = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT();
    pass.stack = (anchor_projection_stack){0};
    read_records(&pass, &cursor, snapshot ? state->snapshot_records : registry->count);
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
            if (!markdown_core_registry_normal_label(&parser->registry, parser->pool, &label, declared) ||
                !markdown_core_registry_declare(&parser->registry, MARKDOWN_CORE_REGISTRY_REFERENCE, declared->data,
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
