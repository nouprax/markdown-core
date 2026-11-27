#include "cross_link.h"
#include "alloc.h"
#include "claim.h"
#include "heading_scanners.h"
#include "citation.h"
#include "heading.h"
#include "directive.h"
#include "inline_internal.h"
#include "block_internal.h"
#include "topic.h"
#include "parts.h"
#include "attributes.h"
#include "ast_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { HEADING_CITATION, HEADING_DIRECTIVE, HEADING_TOPIC, HEADING_ATTRIBUTES, HEADING_CROSS_LINK };
/* A heading whose inline content is parsed (markdown_core_headings_prepare). */
enum { MARKDOWN_CORE_NODE__HEADING_PARSED = MARKDOWN_CORE_NODE__ELEMENT_FIRST };
static const markdown_core_element *const HEADING_PEERS[] = {
    [HEADING_CITATION] = &MARKDOWN_CORE_ELEMENT_CITATION,     [HEADING_DIRECTIVE] = &MARKDOWN_CORE_ELEMENT_DIRECTIVE,
    [HEADING_TOPIC] = &MARKDOWN_CORE_ELEMENT_TOPIC,           [HEADING_ATTRIBUTES] = &MARKDOWN_CORE_ELEMENT_ATTRIBUTES,
    [HEADING_CROSS_LINK] = &MARKDOWN_CORE_ELEMENT_CROSS_LINK, NULL};
typedef enum { ANCHOR_CONTENT, ANCHOR_CITATIONS, ANCHOR_KEY } anchor_projection_kind;
typedef struct {
    markdown_core_node *node;
    anchor_projection_kind kind;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;
/* A heading closes and asks the names service for a name (names.h),
 * which it assigns as the parse ends. */
static bool close_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, markdown_core_node *parent, markdown_core_reader *reader) {
    (void)parent;
    (void)reader;
    /* The end of its text is offered (element.h, markdown_core_follow):
     * what is taken is cut from it. */
    const markdown_core_chunk text = markdown_core_node_text(node);
    node->cut_back += markdown_core_parser_offer(parser, node, text.data, 0, text.len, false);
    markdown_core_parser_name(parser, self, node);
    return true;
}

/* A heading's inline content parses as a paragraph's does, from its seams
 * when an edit's reading leaps in it (blocks.c, THE SEAMS). */
static bool heading_seams(const markdown_core_element_instance *self, const markdown_core_node *node) {
    (void)self;
    (void)node;
    return true;
}

/* The inline content of every heading that settled since the last
 * completion, in source order. The entries the names service reads in
 * source order (names.h) before its `parsed` come before every later one,
 * so only the entries after them are put in order: those that closed since,
 * and those the last completion left because they had not settled. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void headings_prepare(const markdown_core_element_instance *self,
                                                                 markdown_core_parser *parser);

void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    /* Every heading the parse holds is parsed, and none past them is left
     * from a reading put back: there is nothing to prepare. */
    if (markdown_core_names_due(&parser->names)) {
        headings_prepare(self, parser);
    }
}

static void headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    size_t first, count;
    if (!markdown_core_names_order(parser, &first, &count)) {
        return;
    }
    markdown_core_named *const span = markdown_core_names_span(parser, first, count);
    /* Anchors are numbered in source order, whatever order mapped inputs were
     * scheduled and headings closed in. */
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_named *entry = span ? span + (i - first) : markdown_core_names_at(parser, i);
        markdown_core_node *node = entry->node;
        if ((node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) || !markdown_core_parser_settled(parser, node)) {
            continue;
        }
        parser->completing = node;
        markdown_core_parser_parse_root(parser, node);
        node->flags |= MARKDOWN_CORE_NODE__HEADING_PARSED;
        markdown_core_names_read(entry, node->attributes.anchor.len != 0);
    }
    size_t parsed = first;
    while (parsed < count && ((span ? span + (parsed - first) : markdown_core_names_at(parser, parsed))->node->flags &
                              MARKDOWN_CORE_NODE__HEADING_PARSED)) {
        parsed++;
    }
    markdown_core_names_read_to(parser, parsed);
}

/* A heading parsed again (element.h, markdown_core_finish_ops, `reparse`), a
 * candidate of a heading a streaming commit settled: the attributes its last
 * line ends with were taken as it closed, and the tokens that may give them
 * back read the same again, so they stay. The name the
 * names computed for it stays while its attributes write no anchor; its
 * entry learns whether they do, and that the base of its name is to be
 * projected again (markdown_core_names_reread). */
static void reparse_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *node) {
    (void)self;
    markdown_core_parser_parse_root(parser, node);
    markdown_core_names_reread(parser, node, node->attributes.anchor.len != 0);
}

/* The entries `values` holds, `count` of them, let go. */
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
        case MARKDOWN_CORE_NODE_CROSS_EMBED: {
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
        case MARKDOWN_CORE_NODE_EMBED:
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

/* (names.h, the owner's `project`) The projection of a heading's children,
 * its stack the state's, which every projection uses in turn. */
static bool heading_project(const markdown_core_element_instance *self, const markdown_core_node *node, uint32_t from,
                            uint32_t to, markdown_core_strbuf *base, size_t *work) {
    markdown_core_heading_state *state = self->state;
    anchor_projection_stack stack = {state->stack, 0, state->stack_room};
    const bool ok = project_anchor_range(work, node, from, to, base, &stack);
    state->stack = stack.values;
    state->stack_room = stack.capacity;
    return ok;
}

void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    (void)self;
    (void)parser;
    /* An ATX heading's closing sequence ends its text: what may follow it
     * on the line another grammar has taken from the text already
     * (no-flush.md 10.13). */
    if (parent->kind == MARKDOWN_CORE_NODE_HEADING && !parent->as.heading->setext) {
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
                     markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    bufsize_t hashpos;
    int level = 0;
    bufsize_t heading_startpos = reader->nonspace;

    markdown_core_reader_advance(reader, reader->nonspace + matched - reader->offset, false);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
    if (!*container) {
        return false;
    }

    markdown_core_chunk line = {(unsigned char *)markdown_core_reader_rest(reader, 0),
                                markdown_core_reader_length(reader), 0};
    hashpos = markdown_core_chunk_strchr(&line, '#', reader->nonspace);

    while (markdown_core_reader_peek(reader, hashpos) == '#') {
        level++;
        hashpos++;
    }

    (*container)->as.heading->level = level;
    (*container)->as.heading->setext = false;
    (*container)->internal_offset = matched;

    return true;
}
static bool open_setext(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    /* The change of kind cuts the items the paragraph's text opens with
     * first (blocks.c, THE FRONT CUT): what is left becomes the heading. */
    markdown_core_node *parent = markdown_core_parser_parent(parser, *container);
    markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, *container, parent, MARKDOWN_CORE_NODE_HEADING);
    if (result == MARKDOWN_CORE_NODE_SET_KIND_CUT) {
        return true;
    }
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return false;
    }
    (*container)->as.heading->level = matched;
    (*container)->as.heading->setext = true;
    markdown_core_reader_end(reader);
    return true;
}
static bool scan_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         block_start_context *context, block_start *start) {
    (void)self;
    /* The line is read past its first byte only when that byte may begin
     * the heading's line: a `#`, or an underline's `=` or `-`. */
    const unsigned char first = markdown_core_reader_peek(context->reader, context->reader->nonspace);
    if (first == '#' && (start->matched = scan_atx_heading_start(markdown_core_reader_rest(context->reader, 0),
                                                                 markdown_core_reader_length(context->reader),
                                                                 context->reader->nonspace))) {
        start->open = open_atx;
        if (parser->sink && self->peers[HEADING_TOPIC]) {
            const unsigned char *data = markdown_core_reader_rest(context->reader, 0);
            bufsize_t end = context->reader->nonspace, last = markdown_core_reader_length(context->reader);
            while (end < last && data[end] == '#') {
                end++;
            }
            while (last > end && (data[last - 1] == '\n' || data[last - 1] == '\r')) {
                last--;
            }
            if (end == last) {
                markdown_core_topic_report_line(parser, context->reader->nonspace, end);
            }
        }
    } else if (context->paragraph && (first == '=' || first == '-') &&
               markdown_core_reader_blank_from(
                   context->reader, context->reader->nonspace +
                                        markdown_core_reader_run(context->reader, context->reader->nonspace, first))) {
        /* An underline: a run of one of them, and only spaces after it. */
        start->matched = first == '=' ? 1 : 2;
        start->open = open_setext;
    } else {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HEADING;
    return true;
}
static int continue_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_reader *reader, markdown_core_node *container) {
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
 * its old entry or what that entry let go is a witness (names.h, THE
 * STEMS), and which the heading its redo makes it again takes up (names.c,
 * THE NEW BASES): the names service keeps it (markdown_core_names_retire). */
static bool heading_revert(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node, markdown_core_node *parent, uint32_t *kept) {
    markdown_core_heading *value = node->as.heading;
    if (!markdown_core_names_retire(parser, self, node)) {
        return false;
    }
    markdown_core_attributes_free(&node->attributes);
    memset(&node->attributes, 0, sizeof(node->attributes));
    /* What its attributes took from its text is text again. */
    node->cut_back = 0;
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
};

/* A SETEXT UNDERLINE FOLLOWS A PARAGRAPH (element.h, markdown_core_follow):
 * the line makes it a heading (open_setext), which an edit undoes and does
 * again. */
static const markdown_core_node_type HEADING_LEADERS[] = {MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow HEADING_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_LINE,
     .kinds = HEADING_LEADERS,
     .makes = MARKDOWN_CORE_NODE_HEADING,
     .former = heading_former,
     .revert = heading_revert,
     .redo = heading_redo},
    {0},
};

/* (element.h, the finish's `measure`) The measure of the projection of a
 * heading's children, which its seams keep, so that an edit that leaps
 * in it knows how many bytes of its base the children it keeps project to
 * (names.c, THE NEW BASES). */
static bool heading_measure(const markdown_core_element_instance *self, const markdown_core_node *root, uint32_t from,
                            uint32_t to, markdown_core_measure *measure) {
    markdown_core_heading_state *state = self->state;
    size_t work = 0;
    if (!state->scratch.ptr) {
        markdown_core_strbuf_init_zeroed(&state->scratch);
    }
    markdown_core_strbuf_clear(&state->scratch);
    const bool ok = heading_project(self, root, from, to, &state->scratch, &work);
    if (ok) {
        markdown_core_measure_add(measure, state->scratch.ptr, (size_t)state->scratch.size);
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

/* Where a heading keeps the name the names service computes for it. */
static markdown_core_name *heading_name(const markdown_core_node *node) { return &node->as.heading->name; }

const markdown_core_facet MARKDOWN_CORE_HEADING_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_HEADING, .parts = markdown_core_heading_parts, .named = heading_name},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* A heading asks the names service for a name (names.h): its text projects
 * to the name's base. */
static const markdown_core_name_ops HEADING_NAMES = {
    .value = heading_name,
    .project = heading_project,
};

/* What the projections keep, let go with the parse. */
static void headings_release(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    markdown_core_free(state->stack);
    if (state->scratch.ptr) {
        markdown_core_strbuf_free(&state->scratch);
    }
    *state = (markdown_core_heading_state){.stack = NULL};
}

/* The headings' inline content is parsed in the parse's preparation
 * (element.h). */
static const markdown_core_stage_ops HEADING_STAGES = {
    .prepare = markdown_core_headings_prepare,
    .release = headings_release,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .stages = &HEADING_STAGES,
    .names = &HEADING_NAMES,
    .facets = MARKDOWN_CORE_HEADING_FACETS,
    .peers = HEADING_PEERS,
    .name = "heading",
    .state_size = sizeof(markdown_core_heading_state),
    .finish = &HEADING_FINISH,
    .content = &HEADING_CONTENT,
    .inlines = &HEADING_INLINES,
    .opens = &HEADING_OPENS,
    .lines = &HEADING_LINES,
    .edit = &HEADING_EDIT,
    .follows = HEADING_FOLLOWS,
};

/* A heading's record (markdown_core_record_ops): level 1 until its marker
 * says otherwise; its name, whose base it may borrow and whose shown form is
 * its own. A view drops the shown form and borrows the base. */
static void init_heading(markdown_core_node *node) {
    node->as.heading->level = 1;
    markdown_core_strbuf_init_zeroed(&node->as.heading->name.base);
}

static void free_heading(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_heading *heading = node->as.heading;
    if (heading->name.shown) {
        markdown_core_free(heading->name.shown);
        heading->name.shown = NULL;
    }
    if (!markdown_core_strbuf_borrows(&heading->name.base)) {
        markdown_core_strbuf_free(&heading->name.base);
    }
}

static bool view_heading(markdown_core_node_pool *pool, markdown_core_node *copy, const markdown_core_node *node,
                         bool failed) {
    (void)pool;
    (void)node;
    (void)failed;
    markdown_core_heading *heading = copy->as.heading;
    heading->name.shown = NULL;
    if (markdown_core_strbuf_owns(&heading->name.base)) {
        markdown_core_strbuf_borrow(&heading->name.base);
    }
    return true;
}

static bool own_heading(markdown_core_node *copy, const markdown_core_node *node) {
    markdown_core_strbuf *base = &copy->as.heading->name.base;
    if (!markdown_core_strbuf_borrows(base) || base->ptr != node->as.heading->name.base.ptr) {
        return true;
    }
    markdown_core_strbuf own = MARKDOWN_CORE_BUF_INIT();
    markdown_core_strbuf_set(&own, base->ptr, base->size);
    if (own.oom) {
        markdown_core_strbuf_free(&own);
        return false;
    }
    *base = own;
    return true;
}

static void adopt_heading(markdown_core_node *view, markdown_core_node *node) {
    if (markdown_core_strbuf_borrows(&view->as.heading->name.base) &&
        view->as.heading->name.base.ptr == node->as.heading->name.base.ptr) {
        view->as.heading->name.base = node->as.heading->name.base;
        markdown_core_strbuf_borrow(&node->as.heading->name.base);
    }
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_HEADING = {
    .size = sizeof(markdown_core_heading),
    .init = init_heading,
    .free = free_heading,
    .view = view_heading,
    .own = own_heading,
    .adopt = adopt_heading,
};
