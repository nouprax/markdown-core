#include "alloc.h"
#include "claim.h"
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

/* Whether a line ends in the `size` bytes at `bytes`: read a window at a
 * time, so that the first line end stops the reading and no byte is read
 * past the window that holds it. */
static bool heading_line_end(const unsigned char *bytes, bufsize_t size) {
    for (bufsize_t at = 0; at < size; at += 256) {
        const size_t window = (size_t)(size - at < 256 ? size - at : 256);
        if (memchr(bytes + at, '\n', window) || memchr(bytes + at, '\r', window)) {
            return true;
        }
    }
    return false;
}

/* Whether a line of the content `content`, of more bytes than a label may
 * hold, ends past those bytes and before the line endings that end it. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool heading_seams_past(const markdown_core_strbuf *content) {
    bufsize_t end = content->size;
    while (end > MAX_LINK_LABEL_LENGTH + 1 && markdown_core_is_line_end(markdown_core_strbuf_at(content, end - 1))) {
        end--;
    }
    if (end <= MAX_LINK_LABEL_LENGTH + 1) {
        return false;
    }
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_strbuf_span(content, MAX_LINK_LABEL_LENGTH + 1, end, &front, &front_size, &back, &back_size);
    return heading_line_end(front, front_size) || heading_line_end(back, back_size);
}

/* Whether the heading `node` declares nothing its text may name: the bytes
 * of its content before its last line, which alone may end with its
 * attributes, are more than a label may hold. That is, a line ends past
 * the bytes a label may hold and before the line endings that end the
 * content (heading_seams_past). Its inline content parses as a paragraph's
 * does, from its seams when an edit's reading leaps in it (blocks.c, THE
 * SEAMS). */
static inline bool heading_seams(const markdown_core_element_instance *self, const markdown_core_node *node) {
    (void)self;
    return node->content.size > MAX_LINK_LABEL_LENGTH + 1 && heading_seams_past(&node->content);
}

/* The heading's inline content parsed whole, as its preparation and finish
 * parse it, or as a paragraph's when it declares nothing (heading_seams). */
static void heading_parse(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_heading_parse *heading) {
    if (heading_seams(self, heading->node)) {
        markdown_core_parser_parse_root(parser, heading->node);
        return;
    }
    markdown_core_prepare_heading(self, parser, heading);
    markdown_core_finish_heading(parser, heading);
}

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
            if (!heading_seams(self, heading->node)) {
                parser->completing = heading->node;
                markdown_core_prepare_heading(self, parser, heading);
            }
        }
    }
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        markdown_core_node *node = heading->node;
        if ((node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) || !markdown_core_parser_settled(parser, node)) {
            continue;
        }
        parser->completing = node;
        if (heading_seams(self, node)) {
            markdown_core_parser_parse_root(parser, node);
        } else {
            markdown_core_finish_heading(parser, heading);
        }
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

/* THE NAMES. A heading's name is its base, the projection of its text
 * (heading_anchor_base), or the base, a hyphen and a suffix, a number
 * (anchors.md); its node keeps the base and the suffix it shows (node.h,
 * the heading's name), and its anchor is their bytes. A name is read in
 * pieces (pieces.h): the base's, one or two, then the suffix's digits, so
 * no name is joined to be looked up, and its measure, by which the indexes
 * of names file it (map.h, KEYS UNDER A HASH THEIR USER GIVES), is the
 * base's joined with the suffix's. Two names are the same when their bytes
 * are: names whose measures agree are compared, piece by piece, and a
 * stretch both read where one buffer keeps it is not read at all. */
enum { SUFFIX_ROOM = 3 * sizeof(size_t) + 2 };

/* The mark of an index slot whose key is not bytes the slot points at: a
 * name a heading holds, a stem, a count of moves. */
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

/* The base of a heading whose text projects to nothing, which its node
 * borrows. */
static const markdown_core_strbuf SECTION = {.ptr = (unsigned char *)"section", .size = 7};

/* The base the node keeps, in `*pieces`. */
static inline void node_base(const markdown_core_node *node, markdown_core_pieces *pieces) {
    const markdown_core_strbuf *base = &node->as.heading->base;
    markdown_core_pieces_clear(pieces);
    markdown_core_pieces_span(pieces, base, 0, base->size);
}

/* The name of the base heading `node` keeps, hashed. */
static inline void name_node(name_view *name, const markdown_core_node *node) {
    node_base(node, &name->pieces);
    name_hashed(name);
}

/* The anchor the node showed, once read, goes: its name changed. */
static inline void name_unshow(markdown_core_heading *value) {
    if (value->shown) {
        markdown_core_free(value->shown);
        value->shown = NULL;
    }
}

/* The node shows the name of its base and `suffix`. */
static inline void name_show(markdown_core_node *node, uint32_t suffix) {
    markdown_core_heading *value = node->as.heading;
    if (value->suffix != suffix) {
        value->suffix = suffix;
        name_unshow(value);
    }
}

/* The node keeps no base. */
static void base_drop(markdown_core_node *node) {
    markdown_core_heading *value = node->as.heading;
    if (!markdown_core_strbuf_borrows(&value->base)) {
        markdown_core_strbuf_free(&value->base);
    }
    markdown_core_strbuf_init(&value->base, 0);
    value->suffix = 0;
    name_unshow(value);
}

/* A heading parsed again (element.h, markdown_core_finish_ops, `reparse`), a
 * candidate of a heading a streaming commit settled: the attributes its last
 * line ends with are its inline content's and go with it, and the label its
 * text declares, whatever a lookup answers, stays declared. The name the
 * headings computed for it stays while its attributes write no anchor; the
 * heading's entry learns whether they do, and that the base of its name is
 * to be projected again. */
static void reparse_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *node) {
    markdown_core_attributes_free(&node->attributes);
    markdown_core_heading_parse heading = {.node = node};
    heading_parse(self, parser, &heading);
    const bool authored = node->attributes.anchor.len != 0;
    if (authored) {
        base_drop(node);
    }
    markdown_core_heading_parse *entry = parsed_heading(self->state, node->place.start);
    entry->authored = authored;
    entry->named = false;
}

/* The entries `values` holds, `count` of them, let go. */
static void headings_let_go(markdown_core_heading_parse *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        markdown_core_dispose_heading(&values[i]);
    }
}

static void project_anchor_literal(size_t *work, markdown_core_strbuf *base, const unsigned char *text,
                                   bufsize_t length) {
    *work += (size_t)length;
    markdown_core_utf8proc_anchor(base, text, length);
}

static bool push_anchor_projection(anchor_projection_stack *stack, markdown_core_node *node,
                                   anchor_projection_kind kind) {
    if (!node) {
        return true;
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*stack->values)) {
            return false;
        }
        void *values = markdown_core_realloc(stack->values, capacity * sizeof(*stack->values));
        if (!values) {
            return false;
        }
        stack->values = values;
        stack->capacity = capacity;
    }
    stack->values[stack->count++] = (anchor_projection){node, kind};
    return true;
}

/* `node`'s children from `from` up to `to`, pushed last first so that they
 * are projected in order; none for no node. */
static bool push_anchor_range(anchor_projection_stack *stack, const markdown_core_node *node, uint32_t from,
                              uint32_t to, anchor_projection_kind kind) {
    for (uint32_t i = to; node && i > from; i--) {
        if (!push_anchor_projection(stack, node->children[i - 1], kind)) {
            return false;
        }
    }
    return true;
}

/* `node`'s children, pushed last first so that they are projected in order;
 * none for no node. */
static bool push_anchor_children(anchor_projection_stack *stack, const markdown_core_node *node,
                                 anchor_projection_kind kind) {
    return push_anchor_range(stack, node, 0, node ? node->child_count : 0, kind);
}

/* What the stack holds, projected and appended to `base`, the work it took
 * added to `*work`. False when storage could not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool project_anchor_stack(size_t *work,
                                                                                 markdown_core_strbuf *base,
                                                                                 anchor_projection_stack *stack) {
    bool ok = true;
    while (stack->count && ok && !base->oom) {
        anchor_projection projection = stack->values[--stack->count];
        markdown_core_node *node = projection.node;
        (*work)++;
        if (projection.kind == ANCHOR_KEY) {
            project_anchor_literal(work, base, (const unsigned char *)"@", 1);
            project_anchor_literal(work, base, node->as.citation->value.data, node->as.citation->value.len);
            continue;
        }
        if (projection.kind == ANCHOR_CITATIONS) {
            markdown_core_citation_item *item = node->as.citation;
            if (item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
                ok = push_anchor_children(stack, item->suffix, ANCHOR_CONTENT) &&
                     push_anchor_projection(stack, node, ANCHOR_KEY) &&
                     push_anchor_children(stack, item->prefix, ANCHOR_CONTENT);
            } else if (item->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
                ok = push_anchor_projection(stack, node, ANCHOR_KEY);
            }
            continue;
        }
        switch (node->kind) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_CODE:
            project_anchor_literal(work, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_TOPIC:
            project_anchor_literal(work, base, (const unsigned char *)"#", 1);
            project_anchor_literal(work, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_FORMULA: {
            const unsigned char *front, *back;
            bufsize_t front_size, back_size;
            markdown_core_elements_formula_halves(node, &front, &front_size, &back, &back_size);
            project_anchor_literal(work, base, front, front_size);
            project_anchor_literal(work, base, back, back_size);
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
                project_anchor_literal(work, base, cross->label.value.data, cross->label.value.len);
            } else {
                project_anchor_literal(work, base, cross->path.data, cross->path.len);
                if (cross->anchor.has_value) {
                    project_anchor_literal(work, base, cross->anchor.value.data, cross->anchor.value.len);
                }
            }
            break;
        }
        case MARKDOWN_CORE_NODE_CITE:
            ok = push_anchor_children(stack, node, ANCHOR_CITATIONS);
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
            ok = push_anchor_children(stack, node, ANCHOR_CONTENT);
            break;
        case MARKDOWN_CORE_NODE_DIRECTIVE: {
            ok = push_anchor_children(stack, markdown_core_directive_label(node), ANCHOR_CONTENT);
            break;
        }
        default:
            /* HTML, comments and other opaque values contribute no text. */
            break;
        }
    }
    return ok && !base->oom;
}

/* The projection of the children of `root` from `from` up to `to`,
 * appended to `base`. False when storage could not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool project_anchor_range(size_t *work,
                                                                                 const markdown_core_node *root,
                                                                                 uint32_t from, uint32_t to,
                                                                                 markdown_core_strbuf *base,
                                                                                 anchor_projection_stack *stack) {
    stack->count = 0;
    return push_anchor_range(stack, root, from, to, ANCHOR_CONTENT) && project_anchor_stack(work, base, stack);
}

/* The heading's anchor base, projected from its inlines and appended to
 * `base`. The stack's storage is the caller's and serves every heading of the
 * pass, as `base` does; each heading starts it empty. */
static void heading_anchor_base(markdown_core_parser *parser, markdown_core_heading_state *state,
                                markdown_core_node *heading, markdown_core_strbuf *base,
                                anchor_projection_stack *stack) {
    if (parser->error) {
        return;
    }
    if (!project_anchor_range(&state->anchor_work, heading, 0, heading->child_count, base, stack)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    if (base->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
 * let go names, with their parents (heading_rename), and of a group only
 * the headings from the rest point on, until the names behind it are the
 * names they were: so an edit beside one of many headings of one name
 * names few of them. Each stem keeps where its headings start in a zipper
 * of its own about the headings' focus, so that a group's headings come in
 * order from its stems' starts, each found by halves among the headings,
 * and who holds a name is found by halves too (name_held). A name is a stem
 * and a suffix, and a heading's entry names its stem: what the edit lets go
 * and what a pass moves are those numbers, and no name is joined.
 *
 * A stem keeps no bytes: its witness reads them (heading_state.h, THE STEMS),
 * and the index of the stems files each by its measure and compares the
 * bytes of its witness. Every witness holds until the edit's end: the bases
 * of the headings it read stay as they were until then (THE NEW BASES), the
 * nodes it let go live until the next publish does, and the base of a node
 * it made a paragraph again is kept aside (heading_revert). A stem that
 * loses its last heading or child is touched, so that at the edit's end it
 * goes unless it holds one again.
 *
 * The first reopen makes the stems; a parse no edit reopened names its
 * headings as THE COMMITTED NAMES say. */
static inline uint32_t stem_flip(uint32_t start, size_t end) { return (uint32_t)(end - start); }
static inline size_t stem_place(const uint32_t *start) { return *start; }
static inline void stem_leave(markdown_core_heading_stem *stem, size_t first) {
    (void)stem;
    (void)first;
}
MARKDOWN_CORE_ZIPPER_OPS(stem_zipper, markdown_core_heading_starts, uint32_t, stem_flip, stem_place, stem_leave,
                         markdown_core_heading_stem)

static inline markdown_core_heading_stem *stem_of(const markdown_core_heading_names *names, uint32_t id) {
    return &names->stems[id - 1];
}

static inline bool stem_present(const markdown_core_heading_stem *stem) {
    return stem_zipper_count(&stem->starts) != 0;
}

/* The base the node had before the edit: its own, or the one the edit
 * kept aside as it made the node a paragraph again (heading_revert). */
static const markdown_core_strbuf *old_base(const markdown_core_heading_names *names, const markdown_core_node *node) {
    for (size_t i = 0; i < names->retired_count; i++) {
        if (names->retired[i].node == node) {
            return names->retired[i].base;
        }
    }
    return &node->as.heading->base;
}

/* The bytes of a new base (THE NEW BASES), in `*pieces`. */
static void base_pieces(const markdown_core_heading_base *base, markdown_core_pieces *pieces) {
    markdown_core_pieces_clear(pieces);
    if (base->old) {
        markdown_core_pieces_span(pieces, base->old, 0, base->front);
    }
    markdown_core_pieces_add(pieces, base->mid.ptr, base->mid.size);
    if (base->old) {
        markdown_core_pieces_span(pieces, base->old, base->old->size - base->back, base->old->size);
    }
}

/* Whether heading `at` is one the edit's reading made, whose base waits for
 * the edit's end (THE NEW BASES). */
static inline bool made_at(const markdown_core_heading_state *state, size_t at) {
    return at >= state->names->fresh && at < state->headings.front.count;
}

/* The base heading `at` is named with as the edit names: the new one of a
 * heading the reading made, the old one of any other. */
static void member_base(const markdown_core_heading_state *state, size_t at, const markdown_core_heading_parse *heading,
                        markdown_core_pieces *pieces) {
    const markdown_core_heading_names *names = state->names;
    if (made_at(state, at)) {
        base_pieces(&names->bases[at - names->fresh], pieces);
        return;
    }
    const markdown_core_strbuf *base = old_base(names, heading->node);
    markdown_core_pieces_clear(pieces);
    markdown_core_pieces_span(pieces, base, 0, base->size);
}

/* Where the stem's heading `i` starts in the text: its headings in order,
 * the back's measured from the headings' end. */
static size_t stem_place_of(const markdown_core_heading_state *state, const markdown_core_heading_stem *stem,
                            size_t i) {
    const markdown_core_heading_starts *starts = &stem->starts;
    if (i < starts->front.count) {
        return starts->front.values[i];
    }
    return state->headings.end - starts->back[starts->back_count - 1 - (i - starts->front.count)];
}

/* How many of the stem's headings start before `place`, by halves. */
static size_t stem_before(const markdown_core_heading_state *state, const markdown_core_heading_stem *stem,
                          size_t place) {
    size_t low = 0, high = stem_zipper_count(&stem->starts);
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

/* The stem's heading `i`, by halves among the headings, and its place
 * among them in `*at`. */
static markdown_core_heading_parse *stem_member(markdown_core_heading_state *state,
                                                const markdown_core_heading_stem *stem, size_t i, size_t *at) {
    const size_t place = stem_place_of(state, stem, i);
    *at = heading_zipper_from(&state->headings, place);
    markdown_core_heading_parse *heading = heading_at(state, *at);
    assert((*at < state->headings.front.count ? heading->start : state->headings.end - heading->start) == place);
    return heading;
}

/* The stem's bytes, as its witness reads them (THE STEMS), in `*text`. */
static void stem_text(markdown_core_heading_state *state, uint32_t id, markdown_core_pieces *text) {
    const markdown_core_heading_stem *stem = stem_of(state->names, id);
    if (stem_present(stem)) {
        size_t at;
        const markdown_core_heading_parse *heading = stem_member(state, stem, 0, &at);
        member_base(state, at, heading, text);
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
    markdown_core_heading_state *state;
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
static uint32_t stem_find(markdown_core_heading_state *state, const markdown_core_pieces *text,
                          markdown_core_measure measure) {
    const stem_probe probe = {state, text};
    const markdown_core_key_index_slot *slot = markdown_core_key_index_seek(
        &state->names->index, markdown_core_key_index_hash(measure), (bufsize_t)measure.size, stem_holds, &probe);
    return slot ? (uint32_t)slot->value : 0;
}

/* The stem of the name, made when none is kept, which takes a witness at
 * once: a heading or a child. Zero when storage could not be had. */
static uint32_t stem_make(markdown_core_heading_state *state, const markdown_core_pieces *text,
                          markdown_core_measure measure) {
    markdown_core_heading_names *names = state->names;
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
        markdown_core_heading_stem *stems =
            markdown_core_reserve(names->stems, &names->stem_room, names->stem_count + 1, sizeof(*stems));
        if (!stems) {
            return 0;
        }
        names->stems = stems;
        id = (uint32_t)++names->stem_count;
    }
    *stem_of(names, id) = (markdown_core_heading_stem){.measure = measure};
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
static uint32_t stem_parent(markdown_core_heading_state *state, uint32_t id, uint32_t *suffix) {
    const markdown_core_heading_stem *stem = stem_of(state->names, id);
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
static uint32_t name_stem(markdown_core_heading_state *state, uint32_t id, uint32_t suffix) {
    if (!suffix) {
        return id;
    }
    markdown_core_pieces text;
    stem_text(state, id, &text);
    name_view name;
    name_make(&name, &text, stem_of(state->names, id)->measure, suffix);
    return stem_find(state, &name.pieces, name.measure);
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
static bool stem_link(markdown_core_heading_state *state, uint32_t id) {
    markdown_core_heading_names *names = state->names;
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
    markdown_core_heading_stem *own = stem_of(names, id), *up = stem_of(names, parent);
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
static bool stem_unlink(markdown_core_heading_names *names, uint32_t id) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    const uint32_t parent = stem->up;
    if (!parent) {
        return true;
    }
    markdown_core_heading_stem *up = stem_of(names, parent);
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

/* The stem, with no heading and no child, goes, its place then free. */
static void stem_release(markdown_core_heading_names *names, uint32_t id) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    if (!stem->measure.size || stem_present(stem) || stem->first) {
        return;
    }
    markdown_core_key_index_remove(
        &names->index, markdown_core_key_index_seek(&names->index, markdown_core_key_index_hash(stem->measure),
                                                    (bufsize_t)stem->measure.size, stem_is, &id));
    markdown_core_array_free(&stem->starts.front);
    stem_zipper_release_back(&stem->starts);
    *stem = (markdown_core_heading_stem){.next = names->spare};
    names->spare = id;
}

/* A heading of the stem starts at `start` of the text, after each of the
 * stem's headings the front holds. False when storage could not be had. */
static bool stem_add(markdown_core_heading_state *state, uint32_t id, uint32_t start) {
    markdown_core_heading_names *names = state->names;
    markdown_core_heading_stem *stem = stem_of(names, id);
    const bool present = stem_present(stem);
    if (!markdown_core_array_push(&stem->starts.front, start)) {
        return false;
    }
    return (present || stem_link(state, id)) && stem_touch(names, id);
}

/* The stem's headings the back holds that start before `place` of the
 * text, its end `end`, go. False when storage could not be had. */
static bool stem_cut(markdown_core_heading_names *names, uint32_t id, size_t place, size_t end) {
    markdown_core_heading_stem *stem = stem_of(names, id);
    const bool present = stem_present(stem);
    stem->starts.end = end;
    (void)stem_zipper_cut(&stem->starts, place);
    return (!present || stem_present(stem) || stem_unlink(names, id)) && stem_touch(names, id);
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
    names->scratch = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT();
    names->fresh = state->count;
    markdown_core_heading_parse *values = state->headings.front.values;
    for (size_t i = 0; i < state->count; i++) {
        markdown_core_heading_parse *heading = &values[i];
        heading->stem = 0;
        if (heading->authored) {
            continue;
        }
        markdown_core_pieces base;
        node_base(heading->node, &base);
        const uint32_t id = stem_make(state, &base, pieces_measure(&base));
        if (!id || !stem_add(state, id, heading->start)) {
            return false;
        }
        heading->stem = id;
    }
    /* What the stems touched names nothing again: every name holds. */
    names->touched_count = 0;
    markdown_core_key_index_free(&state->taken);
    return true;
}

/* The new bases the last edit left, which its end gave the nodes or which
 * a failed edit leaves, and the bases it kept aside, let go. */
static void bases_free(markdown_core_heading_names *names) {
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

static void stems_free(markdown_core_heading_names *names) {
    if (!names) {
        return;
    }
    for (size_t i = 0; i < names->stem_count; i++) {
        markdown_core_array_free(&names->stems[i].starts.front);
        stem_zipper_release_back(&names->stems[i].starts);
    }
    bases_free(names);
    markdown_core_free(names->stems);
    markdown_core_key_index_free(&names->index);
    markdown_core_free(names->touched);
    markdown_core_free(names->lost);
    markdown_core_free(names->bases);
    markdown_core_free(names->retired);
    markdown_core_strbuf_free(&names->scratch);
    markdown_core_free(names->stack);
    markdown_core_free(names);
}

/* Each heading the move took to the other part takes its stem's focus to
 * `place` as well: those of the front's past `front` before the move, or
 * of the back's past `back`. */
static void stems_follow(markdown_core_heading_state *state, size_t place, size_t front, size_t back) {
    markdown_core_heading_names *names = state->names;
    markdown_core_heading_zipper *headings = &state->headings;
    for (size_t i = back; i < headings->back_count; i++) {
        const uint32_t id = headings->back[i].stem;
        if (id) {
            stem_of(names, id)->starts.end = headings->end;
            (void)stem_zipper_move(&stem_of(names, id)->starts, place, headings->end, stem_of(names, id));
        }
    }
    for (size_t i = front; i < headings->front.count; i++) {
        const uint32_t id = headings->front.values[i].stem;
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
        const uint32_t id = headings->front.values[i].stem;
        if (id) {
            stem_of(names, id)->starts.end = size;
            if (!stem_zipper_ready(&stem_of(names, id)->starts, place, size)) {
                return false;
            }
        }
    }
    for (size_t k = 0; k < taken; k++) {
        const uint32_t id = headings->back[headings->back_count - 1 - k].stem;
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
    state->names->lost_count = 0;
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

/* The heading, which the edit lets go, of stem `id`, its name noted as its
 * stem and suffix (heading_rename), its base the stem's witness while the
 * stem holds no other, and cut from the stem's headings before `place`, in
 * the old text's terms, its end `end`. False when storage could not be
 * had. */
static bool stem_lost(markdown_core_heading_state *state, uint32_t id, const markdown_core_heading_parse *heading,
                      size_t place, size_t end) {
    markdown_core_heading_names *names = state->names;
    markdown_core_heading_lost *lost =
        markdown_core_reserve(names->lost, &names->lost_room, names->lost_count + 1, sizeof(*lost));
    if (!lost) {
        return false;
    }
    names->lost = lost;
    lost[names->lost_count++] = (markdown_core_heading_lost){id, heading->suffix};
    markdown_core_heading_stem *stem = stem_of(names, id);
    stem->relic = old_base(names, heading->node);
    stem->heir = 0;
    return stem_cut(names, id, place, end);
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
        const markdown_core_heading_parse *heading = &headings->back[headings->back_count - 1 - k];
        if (heading->stem && !stem_lost(state, heading->stem, heading, old, headings->end)) {
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
        const markdown_core_heading_parse *heading = &headings->back[i];
        if (heading->stem && !stem_lost(state, heading->stem, heading, SIZE_MAX, headings->end)) {
            return false;
        }
    }
    headings_let_go(headings->back, headings->back_count);
    headings->back_count = 0;
    return true;
}

/* Release the headings and the names they took. The anchors are assigned
 * by then, or the parse failed; either way nothing reads them again. */
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

/* THE COMMITTED NAMES. A heading's anchor is the first name of its sequence
 * -- its base, then the base with -1, -2 and so on -- that no explicit anchor
 * reserves and no heading before it was given (anchors.md). The names taken
 * are one index. A name's entry holds who takes it, in `extra`: zero for an
 * explicit anchor, a heading's place in the collection plus one otherwise;
 * and in `value` the suffix a heading projecting to the name tries next. An
 * anchor's entry keys the label of the roster record that reserves it; a
 * heading's is read from the heading, its node's base and its suffix.
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
    /* What a heading's text projects to, and the projection's stack, which
     * every heading of the pass uses in turn. */
    markdown_core_strbuf name;
    anchor_projection_stack stack;
} name_pass;

/* The suffix of the name a heading holds in an index: its committed one,
 * but in a snapshot's overlay the one the snapshot gave a committed heading
 * it renamed (A COMMITTED HEADING UNDER ANOTHER NAME). */
static inline uint32_t holder_suffix(const markdown_core_heading_parse *heading, bool overlay) {
    return overlay && heading->renamed ? heading->node->as.heading->suffix : heading->suffix;
}

/* What the index of the names taken compares: the name looked for, and
 * whether the slots are the overlay's. */
typedef struct {
    markdown_core_heading_state *state;
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
    const markdown_core_heading_parse *holder = heading_at(look->state, slot->extra - 1);
    const markdown_core_strbuf *base = &holder->node->as.heading->base;
    const uint32_t suffix = holder_suffix(holder, look->overlay);
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
    node_base(holder->node, &held);
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
    pass->state->anchor_work += (size_t)name->pieces.size + 1;
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

/* A COMMITTED HEADING UNDER ANOTHER NAME. A snapshot that displaces a
 * committed heading gives its node the suffix the overlay gives it, and its
 * entry keeps the committed one. The snapshot lists the heading; the next
 * one gives the node its committed suffix back unless it displaces the
 * heading too. */
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

/* Heading `at` takes the name of its base and `suffix`: its node shows it,
 * and its entry keeps it unless an overlay names a committed heading (A
 * COMMITTED HEADING UNDER ANOTHER NAME). False with the parse failed. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool name_take(name_pass *pass, size_t at, uint32_t suffix) {
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_parse *heading = heading_at(state, at);
    name_show(heading->node, suffix);
    if (pass->beneath && at < state->named) {
        return rename_committed(pass->parser, state, at);
    }
    heading->suffix = suffix;
    return true;
}

/* The entry `entry`, vacant or held by a later heading, names heading
 * `at`'s name. That heading, as its place plus one, or zero. */
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

/* Heading `at`, after every heading the pass named, takes the first name of
 * its sequence that is vacant: `name`, the name of its base,
 * then the base with the suffix the base's entry keeps, which it advances,
 * so a run of headings projecting to one name tries each suffix once. Such a
 * heading is not committed under an overlay. */
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

/* Heading `at`, displaced from the name it holds, takes the first name past
 * it in its sequence that is vacant or that a later heading holds, and so
 * on for each heading displaced in turn, each later than the last. */
static void displace(name_pass *pass, size_t at) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    for (size_t holder = at + 1; holder && !parser->error;) {
        at = holder - 1;
        const markdown_core_heading_parse *heading = heading_at(state, at);
        size_t suffix = holder_suffix(heading, pass->beneath != NULL);
        name_view name;
        name_node(&name, heading->node);
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
    name_view name;
    markdown_core_pieces_clear(&name.pieces);
    markdown_core_pieces_add(&name.pieces, label.data, label.len);
    name_hashed(&name);
    if (record->withdraws) {
        assert(!pass->beneath);
        if (!record->live) {
            pass->state->anchor_work += (size_t)label.len + 1;
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

/* The heading's node keeps the base its text projects to, unless it keeps
 * it already. */
static void base_read(name_pass *pass, markdown_core_heading_parse *heading) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_strbuf *name = &pass->name;
    markdown_core_strbuf_clear(name);
    heading_anchor_base(parser, pass->state, heading->node, name, &pass->stack);
    if (parser->error) {
        return;
    }
    /* A heading whose text projects to nothing borrows the base "section";
     * any other copies the projection, so the pass keeps its storage for the
     * next heading's. */
    markdown_core_heading *value = heading->node->as.heading;
    const markdown_core_strbuf *read = name->size ? name : &SECTION;
    markdown_core_pieces base;
    node_base(heading->node, &base);
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
    heading->named = true;
}

/* The headings from `first` to `end` that do not author their anchors
 * named, each after every heading the pass named before it. A heading whose
 * node keeps its base reads it there. */
static void name_headings(name_pass *pass, size_t first, size_t end) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    for (size_t at = first; at < end && !parser->error; at++) {
        markdown_core_heading_parse *heading = heading_at(state, at);
        if (heading->authored) {
            continue;
        }
        if (heading->named) {
            state->anchor_work += (size_t)heading->node->as.heading->base.size;
        } else {
            base_read(pass, heading);
        }
        if (!parser->error) {
            name_view name;
            name_node(&name, heading->node);
            take_last(pass, at, &name);
        }
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
        if (!heading->renamed) {
            name_show(heading->node, heading->suffix);
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

/* The committed names let go, for a pass that starts empty, which names
 * every heading again. */
static void names_reset(markdown_core_heading_state *state) {
    state->renamed_count = 0;
    markdown_core_key_index_free(&state->taken);
    state->records_at = state->records_read = state->named = 0;
}

/* Whether a heading that starts before `place` holds the name of the stem
 * `id` and `suffix`. Only two can (THE NAMES AN EDIT MOVES): the first of
 * the name's own stem, when it holds its base, and one of the parent's
 * that holds the name's suffix. The suffixes of a stem's headings rise in
 * order, from its base at most at the first: each takes its base, which
 * stays taken, or the next of its stem's suffixes that is free. */
static bool name_held(markdown_core_heading_state *state, uint32_t id, uint32_t suffix, size_t place) {
    markdown_core_heading_names *names = state->names;
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
    const markdown_core_heading_stem *stem = stem_of(names, up);
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
 * and suffix, how many more of its headings take the name than took it,
 * over the headings the pass named and those the edit let go, each count
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

/* A stem's next heading the pass names: where it starts, the stem, and
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

/* A pass over the groups of the stems an edit touched (heading_rename):
 * the heap of its stems' cursors, least place first, and the mark of the
 * stems it holds. */
typedef struct {
    markdown_core_heading_state *state;
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

/* The stem `id` joins the pass from its first heading at or past `place`,
 * trying the suffix past its last heading before that took one, unless the
 * pass holds it or it holds no heading. False when storage could not be
 * had. */
static bool stem_join(stem_heap *pass, uint32_t id, size_t place) {
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_stem *stem = stem_of(state->names, id);
    if (!stem->measure.size || stem->seen == pass->mark) {
        return true;
    }
    stem->seen = pass->mark;
    const size_t first = stem_before(state, stem, place);
    if (first == stem_zipper_count(&stem->starts)) {
        return true;
    }
    stem->suffix = 1;
    for (size_t k = first; k > 0; k--) {
        size_t at;
        const markdown_core_heading_parse *heading = stem_member(state, stem, k - 1, &at);
        if (heading->suffix) {
            stem->suffix = (size_t)heading->suffix + 1;
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
 * MOVES): their headings in order, each taking the first name of its
 * sequence that no anchor standing reserves and no heading before it
 * holds, past the suffixes its base's earlier headings tried. The pass
 * starts at the first heading of the stems the reading touched and of
 * their parents, where the rest point is, as those before it keep their
 * names, unless an anchor the edit made or let go names a stem of them,
 * which moves the names of any: then it starts at their first. A stem
 * joins it where a heading of the pass takes or lets go a name the stem is
 * the base of, or the parent's where one of its headings takes or lets go
 * its base: only a name binds two stems, so a group joins as its names
 * move, and an edit beside one of many stems of one parent names few of
 * them. It ends once every heading the reading made is named and the
 * headings named, with those the edit let go, took the names they took
 * before, stem by stem: the old headings after them then take theirs
 * again. */
static void heading_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_names *names = state->names;
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
        const markdown_core_heading_lost lost = names->lost[i];
        if (stem_of(names, lost.stem)->seen == heap.mark) {
            ok = name_move(&moves, lost.stem, lost.suffix, -1) && name_join(&heap, lost.stem, lost.suffix, from);
        }
    }
    while (ok && heap.count && !parser->error) {
        const stem_cursor cursor = heap.heap[0];
        markdown_core_heading_stem *stem = stem_of(names, cursor.stem);
        size_t at;
        markdown_core_heading_parse *heading = stem_member(state, stem, cursor.next, &at);
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
            const bool reserved = markdown_core_roster_find_name(&parser->roster, &name.pieces, name.measure, &failed);
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
        const uint32_t old = heading->suffix;
        if (made || old != suffix) {
            /* The stems the old name and the new are the bases of join, and
             * the parent when the heading takes or lets go its base. */
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
        heading->suffix = suffix;
        name_show(heading->node, suffix);
        /* A stem that joined starts past the heading, which stays on top. */
        assert(heap.heap[0].stem == cursor.stem && heap.heap[0].next == cursor.next);
        if (cursor.next + 1 < stem_zipper_count(&stem->starts)) {
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

/* THE NEW BASES. The base of a heading the edit's reading made waits for
 * the edit's end, while its node keeps the base it had, which a stem may
 * read as its witness: a heading the reading read whole projects its text
 * whole, and one whose reading leapt in it (blocks.c, THE SEAMS) keeps its
 * old base but for the projection of the children its parse made, which
 * takes the place of the old bytes between those of the children it kept,
 * as the measures its seams keep say how many (markdown_core_parser_leapt).
 * Its name is read and measured from those parts, so an edit in a long
 * heading projects only what it changed, and compares only that with the
 * old base (pieces.h). The edit's end gives each node its new base: in
 * place, where only the bytes it changed move (buffer.h,
 * markdown_core_strbuf_split_replace). */
static void base_new(name_pass *pass, const markdown_core_heading_parse *heading, markdown_core_heading_base *base) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_node *node = heading->node;
    *base = (markdown_core_heading_base){.mid = MARKDOWN_CORE_BUF_INIT()};
    const markdown_core_strbuf *old = old_base(state->names, node);
    markdown_core_leapt leapt = {MARKDOWN_CORE_MEASURE_NONE, MARKDOWN_CORE_MEASURE_NONE, 0, 0};
    uint32_t from = 0, to = node->child_count;
    if (old->size && markdown_core_parser_leapt(parser, node, &leapt) &&
        (size_t)leapt.front.size + (size_t)leapt.back.size <= (size_t)old->size) {
        *base = (markdown_core_heading_base){old, (bufsize_t)leapt.front.size, (bufsize_t)leapt.back.size,
                                             MARKDOWN_CORE_BUF_INIT(), MARKDOWN_CORE_MEASURE_NONE};
        from = leapt.from;
        to = leapt.to;
    }
    if (!project_anchor_range(&state->anchor_work, node, from, to, &base->mid, &pass->stack)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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

/* Each node the edit's reading made a heading of takes its new base (THE
 * NEW BASES), and one whose attributes write its anchor keeps none. False
 * when storage could not be had. */
static bool bases_commit(markdown_core_heading_state *state) {
    markdown_core_heading_names *names = state->names;
    bool ok = true;
    for (size_t i = names->fresh; i < state->count; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        markdown_core_heading_base *base = &names->bases[i - names->fresh];
        markdown_core_heading *value = heading->node->as.heading;
        if (heading->authored) {
            base_drop(heading->node);
            continue;
        }
        markdown_core_strbuf *old = (markdown_core_strbuf *)base->old;
        if (old && !markdown_core_strbuf_borrows(old)) {
            ok = ok && markdown_core_strbuf_split_replace(old, base->front, old->size - base->back, base->mid.ptr,
                                                          base->mid.size);
            if (old != &value->base) {
                /* A base kept aside (heading_revert) is the node's again. */
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
            node_base(heading->node, &now);
            if (!markdown_core_pieces_are(&now, base->mid.ptr, base->mid.size)) {
                if (markdown_core_strbuf_borrows(&value->base)) {
                    markdown_core_strbuf_init(&value->base, 0);
                }
                markdown_core_strbuf_swap(&value->base, &base->mid);
            }
        }
        name_unshow(value);
        heading->named = true;
    }
    return ok;
}

/* An edit's headings named (THE NAMES AN EDIT MOVES): each the reading
 * made takes its new base (THE NEW BASES) and joins its stem, the stems an
 * anchor it made or let go names are touched with their parents, and the
 * groups of the stems touched are named again; then the stems with neither
 * heading nor child go, and the nodes take their new bases. */
static void headings_rename(name_pass *pass) {
    markdown_core_parser *parser = pass->parser;
    markdown_core_heading_state *state = pass->state;
    markdown_core_heading_names *names = state->names;
    const size_t made = state->count - names->fresh;
    markdown_core_heading_base *bases =
        made ? markdown_core_reserve(names->bases, &names->base_room, made, sizeof(*bases)) : names->bases;
    if (made && !bases) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else {
        names->bases = bases;
    }
    for (size_t i = names->fresh; i < state->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        markdown_core_heading_base *base = &names->bases[names->base_count++];
        *base = (markdown_core_heading_base){.mid = MARKDOWN_CORE_BUF_INIT()};
        heading->stem = 0;
        if (!heading->authored) {
            base_new(pass, heading, base);
        }
    }
    for (size_t i = names->fresh; i < state->count && !parser->error; i++) {
        markdown_core_heading_parse *heading = heading_at(state, i);
        if (heading->authored) {
            continue;
        }
        const markdown_core_heading_base *base = &names->bases[i - names->fresh];
        markdown_core_pieces text;
        base_pieces(base, &text);
        const uint32_t id = stem_make(state, &text, base->measure);
        if (!id || !stem_add(state, id, heading->start)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        heading->stem = id;
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
        heading_rename(pass);
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

/* Each heading's anchor (THE COMMITTED NAMES): an authored one stays, and a
 * computed one is the heading's base and the suffix chosen; only a heading
 * whose name changes is written. A snapshot moves the committed names on,
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

/* A setext heading read its lines as a paragraph, which its underline made
 * a heading (THE FORMER KIND). */
static markdown_core_node_type heading_former(const markdown_core_element_instance *self,
                                              const markdown_core_node *node) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_HEADING && node->as.heading->setext ? MARKDOWN_CORE_NODE_PARAGRAPH
                                                                                : MARKDOWN_CORE_NODE_NONE;
}

/* The paragraph it was, its level kept for its underline's redo, without
 * the attributes its inline content gave it, as a new paragraph is. Its
 * base is kept aside to the edit's end, which the stem it named reads while
 * its old entry or what that entry let go is a witness (THE STEMS), and
 * which the heading its redo makes it again takes up (THE NEW BASES). */
static bool heading_revert(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node, markdown_core_node *parent, uint32_t *kept) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_names *names = state->names;
    markdown_core_heading *value = node->as.heading;
    if (names && markdown_core_strbuf_owns(&value->base)) {
        markdown_core_heading_retired *retired =
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
        names->retired[names->retired_count++] = (markdown_core_heading_retired){node, base};
        /* A stem whose last heading to go was this one reads the base
         * where it is kept now. */
        for (size_t i = 0; i < names->lost_count; i++) {
            markdown_core_heading_stem *stem = stem_of(names, names->lost[i].stem);
            if (stem->relic == &value->base) {
                stem->relic = base;
            }
        }
    }
    markdown_core_attributes_free(&node->attributes);
    memset(&node->attributes, 0, sizeof(node->attributes));
    const uint32_t level = (uint32_t)value->level;
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_PARAGRAPH);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    *kept = level;
    return true;
}

/* The heading its old underline made it, as open_setext did. */
static bool heading_redo(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, markdown_core_node *parent, uint32_t kept) {
    (void)self;
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_HEADING);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    node->as.heading->level = (int)kept;
    node->as.heading->setext = true;
    return true;
}

/* A heading the splice closed as its old reading did, its old flags back,
 * parses its inline content again, as the reading read it again. */
static void heading_spliced(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *node) {
    (void)self;
    (void)parser;
    node->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__HEADING_PARSED;
}

static const markdown_core_edit_ops HEADING_EDIT = {
    .spliced = heading_spliced,
    .former = heading_former,
    .revert = heading_revert,
    .redo = heading_redo,
};

/* The anchor a name shows when it is not its base whole (THE NAMES), as a
 * claim holds it. */
typedef struct {
    bufsize_t size;
    unsigned char text[];
} shown_anchor;

bool markdown_core_heading_anchor(const markdown_core_node *node, markdown_core_chunk *shows) {
    if (node->kind != MARKDOWN_CORE_NODE_HEADING || node->attributes.anchor.len) {
        *shows = node->attributes.anchor;
        return true;
    }
    markdown_core_heading *value = node->as.heading;
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
        node_base(node, &pieces);
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

/* (element.h, the finish's `measure`) The measure of the projection of a
 * long heading's children, which its seams keep, so that an edit that leaps
 * in it knows how many bytes of its base the children it keeps project to
 * (THE NEW BASES). */
static bool heading_measure(const markdown_core_element_instance *self, const markdown_core_node *root, uint32_t from,
                            uint32_t to, markdown_core_measure *measure) {
    markdown_core_heading_state *state = self->state;
    markdown_core_heading_names *names = state->names;
    if (!names) {
        return false;
    }
    anchor_projection_stack stack = {names->stack, 0, names->stack_room};
    markdown_core_strbuf_clear(&names->scratch);
    const bool ok = project_anchor_range(&state->anchor_work, root, from, to, &names->scratch, &stack);
    names->stack = stack.values;
    names->stack_room = stack.capacity;
    if (ok) {
        markdown_core_measure_add(measure, names->scratch.ptr, (size_t)names->scratch.size);
    }
    return ok;
}

static const markdown_core_finish_ops HEADING_FINISH = {
    .reparse = reparse_inlines,
    .deferred = true,
    .seams = heading_seams,
    .measure = heading_measure,
};

static const markdown_core_content_ops HEADING_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inlines = true,
};

static const markdown_core_inline_ops HEADING_INLINES = {
    .enter = markdown_core_heading_begin_inlines,
    .claim = markdown_core_heading_claim_tail,
};

static const markdown_core_open_ops HEADING_OPENS = {
    .indent = 3,
    .scan = scan_heading,
    /* ATX `#`; a setext underline of `=` or `-`. */
    .scan_needs = {.bytes = "#=-"},
};

static const markdown_core_line_ops HEADING_LINES = {
    .matches = continue_heading,
    .close = close_heading,
    .blank_opaque = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .peers = HEADING_PEERS,
    .name = "heading",
    .state_size = sizeof(markdown_core_heading_state),
    .kept_size = offsetof(markdown_core_heading_state, headings),
    .run_state_size = sizeof(markdown_core_heading_run),
    .finish = &HEADING_FINISH,
    .content = &HEADING_CONTENT,
    .inlines = &HEADING_INLINES,
    .opens = &HEADING_OPENS,
    .lines = &HEADING_LINES,
    .edit = &HEADING_EDIT,
};
