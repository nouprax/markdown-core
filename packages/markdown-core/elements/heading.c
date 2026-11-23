#include "alloc.h"
#include "claim.h"
#include "heading_scanners.h"
#include "heading.h"
#include "services.h"
#include "inline_internal.h"
#include "block_internal.h"
#include "parts.h"
#include "attributes.h"
#include "ast_internal.h"

/* The services this element asks, as `self->needs` holds them. */
enum { HEADING_LABELS, HEADING_TOPIC_LINES };
static const char *const HEADING_NEEDS[] = {
    [HEADING_LABELS] = MARKDOWN_CORE_SERVICE_LABELS, [HEADING_TOPIC_LINES] = MARKDOWN_CORE_SERVICE_TOPIC_LINES, NULL};
/* A heading whose inline content is parsed (markdown_core_headings_prepare). */
enum { MARKDOWN_CORE_NODE__HEADING_PARSED = MARKDOWN_CORE_NODE__ELEMENT_FIRST };
/* The spaces this element files labels in, as `self->spaces` numbers them:
 * the reference labels', a heading's text declaring one (a space is its
 * name, element.h). */
enum { HEADING_REFERENCE };
static const markdown_core_space HEADING_REFERENCE_SPACE = {.name = "reference", .folds = true};
static const markdown_core_space *const HEADING_SPACES[] = {[HEADING_REFERENCE] = &HEADING_REFERENCE_SPACE, NULL};
/* A PIECE OF A PROJECTION still to read: a node, or the bytes of a text
 * piece when it has none. */
typedef struct {
    const markdown_core_node *node;
    const unsigned char *data;
    bufsize_t size;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;
/* A heading closes and asks the names service for a name (names.h),
 * which it assigns as the parse ends; the heading declares its label as
 * its inline content is parsed (markdown_core_headings_prepare). */
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

static markdown_core_inline_state *prepare_heading(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, markdown_core_node *node);
static void finish_heading(markdown_core_parser *parser, markdown_core_inline_state *parked);

/* The heading's inline content parsed whole, as its preparation and finish
 * parse it, or as a paragraph's when it declares nothing (heading_seams). */
static void heading_parse(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node) {
    if (heading_seams(self, node)) {
        markdown_core_parser_parse_root(parser, node);
        return;
    }
    finish_heading(parser, prepare_heading(self, parser, node));
}

/* The inline content of every heading that settled since the last
 * completion: first, in source order, each one's inlines up to its
 * declaration dependency and the implicit reference it declares, then the
 * rest of their inlines. The entries the names service reads in source
 * order (names.h) before its `parsed` come before every later one, so only
 * the entries after them are put in order: those that closed since, and
 * those the last completion left because they had not settled. The inline
 * states the first pass leaves parked wait in the state's list, by entry. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void headings_prepare(const markdown_core_element_instance *self,
                                                                 markdown_core_parser *parser);

void markdown_core_headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    /* Every heading the parse holds is parsed, and none past them is left
     * from a reading put back: there is nothing to prepare. */
    if (markdown_core_names_due(&parser->names)) {
        headings_prepare(self, parser);
    }
}

/* The inline state that waits for entry `at`, or NULL; taken from the list. */
static markdown_core_inline_state *held_take(markdown_core_heading_state *state, size_t *next, size_t at) {
    if (*next < state->held_count && state->held[*next].at == at) {
        return state->held[(*next)++].state;
    }
    return NULL;
}

static void headings_prepare(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    size_t first, count;
    if (!markdown_core_names_order(parser, &first, &count)) {
        return;
    }
    markdown_core_named *const span = markdown_core_names_span(parser, first, count);
    state->held_count = 0;
    /* Anchors are numbered in source order, whatever order mapped inputs were
     * scheduled and headings closed in. */
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_node *node = (span ? span + (i - first) : markdown_core_names_at(parser, i))->node;
        if (!(node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) && markdown_core_parser_settled(parser, node) &&
            !heading_seams(self, node)) {
            parser->completing = node;
            markdown_core_inline_state *parked = prepare_heading(self, parser, node);
            if (!parked) {
                continue;
            }
            markdown_core_heading_held *list =
                markdown_core_reserve(state->held, &state->held_room, state->held_count + 1, sizeof(*list));
            if (!list) {
                finish_heading(NULL, parked);
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
            state->held = list;
            list[state->held_count++] = (markdown_core_heading_held){i, parked};
        }
    }
    size_t next = 0;
    for (size_t i = first; i < count && !parser->error; i++) {
        markdown_core_named *entry = span ? span + (i - first) : markdown_core_names_at(parser, i);
        markdown_core_node *node = entry->node;
        if ((node->flags & MARKDOWN_CORE_NODE__HEADING_PARSED) || !markdown_core_parser_settled(parser, node)) {
            continue;
        }
        parser->completing = node;
        if (heading_seams(self, node)) {
            markdown_core_parser_parse_root(parser, node);
        } else {
            finish_heading(parser, held_take(state, &next, i));
        }
        node->flags |= MARKDOWN_CORE_NODE__HEADING_PARSED;
        markdown_core_names_read(entry, node->attributes.anchor.len != 0);
    }
    /* A failed parse lets go the inline states it held for the headings. */
    for (; next < state->held_count; next++) {
        finish_heading(NULL, state->held[next].state);
    }
    state->held_count = 0;
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
 * back read the same again, so they stay; the label its text declares,
 * whatever a lookup answers, stays declared. The name the
 * names computed for it stays while its attributes write no anchor; its
 * entry learns whether they do, and that the base of its name is to be
 * projected again (markdown_core_names_reread). */
static void reparse_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *node) {
    heading_parse(self, parser, node);
    markdown_core_names_reread(parser, node, node->attributes.anchor.len != 0);
}

/* The entries `values` holds, `count` of them, let go. */
static void project_anchor_literal(size_t *work, markdown_core_strbuf *base, const unsigned char *text,
                                   bufsize_t length) {
    *work += (size_t)length;
    markdown_core_utf8proc_anchor(base, text, length);
}

static bool push_anchor_projection(anchor_projection_stack *stack, anchor_projection piece) {
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
    stack->values[stack->count++] = piece;
    return true;
}

/* `node`'s children from `from` up to `to`, pushed last first so that they
 * are projected in order; none for no node. */
static bool push_anchor_range(anchor_projection_stack *stack, const markdown_core_node *node, uint32_t from,
                              uint32_t to) {
    for (uint32_t i = to; node && i > from; i--) {
        if (!push_anchor_projection(stack, (anchor_projection){node->children[i - 1], NULL, 0})) {
            return false;
        }
    }
    return true;
}

/* WHAT A NODE READS AS (element.h, markdown_core_words) when its kind's
 * `words` says it: bytes it gives before any node go to the name at once,
 * and the rest is pushed in order and turned last first once it has said
 * it all. */
typedef struct {
    anchor_projection_stack *stack;
    markdown_core_strbuf *base;
    size_t *work;
    size_t first;
} S_reading;

static bool S_words_text(void *context, const unsigned char *data, bufsize_t size) {
    S_reading *reading = context;
    if (size <= 0) {
        return true;
    }
    if (reading->stack->count == reading->first) {
        project_anchor_literal(reading->work, reading->base, data, size);
        return true;
    }
    return push_anchor_projection(reading->stack, (anchor_projection){NULL, data, size});
}

static bool S_words_node(void *context, const markdown_core_node *node) {
    S_reading *reading = context;
    return !node || push_anchor_projection(reading->stack, (anchor_projection){node, NULL, 0});
}

/* What the stack holds, projected and appended to `base`, the work it took
 * added to `*work`: each node as its kind's facet says it reads in a name
 * (element.h, `reads`), whichever element shows the kind. False when
 * storage could not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool project_anchor_stack(
    const markdown_core_element_instance *self, size_t *work, markdown_core_strbuf *base,
    anchor_projection_stack *stack) {
    S_reading reading = {stack, base, work, 0};
    const markdown_core_words words = {&reading, S_words_text, S_words_node};
    bool ok = true;
    while (stack->count && ok && !base->oom) {
        anchor_projection piece = stack->values[--stack->count];
        if (!piece.node) {
            project_anchor_literal(work, base, piece.data, piece.size);
            continue;
        }
        (*work)++;
        const markdown_core_node *node = piece.node;
        const markdown_core_facet *facet = markdown_core_instance_facet(self, markdown_core_kind_public(node->kind));
        if (!facet) {
            continue;
        }
        if (facet->words) {
            reading.first = stack->count;
            ok = facet->words(node, &words);
            for (size_t a = reading.first, b = stack->count; ok && a + 1 < b; a++, b--) {
                anchor_projection swap = stack->values[a];
                stack->values[a] = stack->values[b - 1];
                stack->values[b - 1] = swap;
            }
        } else if (facet->reads == MARKDOWN_CORE_READS_LITERAL) {
            project_anchor_literal(work, base, node->as.literal->data, node->as.literal->len);
        } else if (facet->reads == MARKDOWN_CORE_READS_CHILDREN) {
            ok = push_anchor_range(stack, node, 0, node->child_count);
        }
    }
    return ok && !base->oom;
}

/* The projection of the children of `root` from `from` up to `to`,
 * appended to `base`. False when storage could not be had. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool project_anchor_range(
    const markdown_core_element_instance *self, size_t *work, const markdown_core_node *root, uint32_t from,
    uint32_t to, markdown_core_strbuf *base, anchor_projection_stack *stack) {
    stack->count = 0;
    return push_anchor_range(stack, root, from, to) && project_anchor_stack(self, work, base, stack);
}

/* (names.h, the owner's `project`) The projection of a heading's children,
 * its stack the state's, which every projection uses in turn. */
static bool heading_project(const markdown_core_element_instance *self, const markdown_core_node *node, uint32_t from,
                            uint32_t to, markdown_core_strbuf *base, size_t *work) {
    markdown_core_heading_state *state = self->state;
    anchor_projection_stack stack = {state->stack, 0, state->stack_room};
    const bool ok = project_anchor_range(self, work, node, from, to, base, &stack);
    state->stack = stack.values;
    state->stack_room = stack.capacity;
    return ok;
}

/* The heading's inline content parsed up to its declaration dependency, and
 * the label it declares: the inline state that waits for the rest, or NULL
 * when none is left (finish_heading). */
static markdown_core_inline_state *prepare_heading(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, node, true, &inline_state);
    while (!parser->error && !inline_state.error) {
        unsigned char c = markdown_core_inline_peek_char(&inline_state);
        /* Attribute ownership and opaque tokens are decided by the same
         * cursor as every inline. A live bracket makes this declaration
         * unwritable as a reference label; only its remaining inlines depend
         * on the document's completed symbol table. */
        if ((inline_state.delims.last && delimiter_last(&inline_state.delims)->kind == DELIMITER_FIELD) ||
            (inline_state.pos >= inline_state.opaque_end &&
             (c == '[' || c == ']' ||
              ((c == '!' || c == '^') && markdown_core_inline_peek_char_n(&inline_state, 1) == '[')))) {
            markdown_core_inline_state *parked = markdown_core_alloc(1, sizeof(inline_state));
            if (parked) {
                *parked = inline_state;
                return parked;
            }
            inline_state.error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            break;
        }
        if (markdown_core_inline_is_eof(&inline_state) || !markdown_core_inline_parse_inline(parser, &inline_state)) {
            break;
        }
    }
    if (!parser->error && !inline_state.error) {
        const markdown_core_heading_run *run = markdown_core_run_state(&inline_state, self);
        /* A heading whose text gave its tail back has the whole text. */
        markdown_core_chunk label = {inline_state.input.data, node->cut_back ? run->label_end : inline_state.input.len,
                                     0};
        /* A heading whose text is writable as a reference label declares
         * it, normalized: an occurrence naming it resolves to the heading
         * when no Reference declares it. It declares it once: a heading
         * parsed again has. */
        markdown_core_chunk *declared = &node->as.heading->label;
        const markdown_core_need labels = self->needs[HEADING_LABELS];
        if (!declared->len && labels.ops && label.len > 0 && label.len <= MAX_LINK_LABEL_LENGTH &&
            ((const markdown_core_label_ops *)labels.ops)->length(labels.instance, label.data, label.len) ==
                label.len) {
            markdown_core_parser_declare(parser, self->spaces[HEADING_REFERENCE], &label, node->place.start, declared);
        }
    }
    /* The run is read to its end: it finishes as every run does. */
    markdown_core_inline_finish_inlines(parser, &inline_state);
    return NULL;
}

/* The rest of the inline content the preparation left `parked`, then let
 * go; with no parser, only let go, as a failed parse does. */
static void finish_heading(markdown_core_parser *parser, markdown_core_inline_state *parked) {
    if (!parked) {
        return;
    }
    if (parser) {
        markdown_core_inline_finish_inlines(parser, parked);
    } else {
        markdown_core_inline_clear_inlines(parked);
    }
    markdown_core_free(parked);
}

void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    (void)parser;
    markdown_core_heading_run *run = markdown_core_run_state(inline_state, self);
    /* An ATX heading's closing sequence ends its text: what may follow it
     * on the line another grammar has taken from the text already
     * (no-flush.md 10.13). */
    if (parent->kind == MARKDOWN_CORE_NODE_HEADING && !parent->as.heading->setext) {
        bufsize_t hashes = markdown_core_inline_size(inline_state);
        while (hashes > 0 && markdown_core_inline_char_at(inline_state, hashes - 1) == '#') {
            hashes--;
        }
        if (hashes < markdown_core_inline_size(inline_state) &&
            (hashes == 0 || markdown_core_is_space_or_tab(markdown_core_inline_char_at(inline_state, hashes - 1)))) {
            markdown_core_inline_shorten(inline_state, hashes);
        }
    }
    run->label_end = markdown_core_inline_size(inline_state);
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
        if (parser->sink && self->needs[HEADING_TOPIC_LINES].ops) {
            const unsigned char *data = markdown_core_reader_rest(context->reader, 0);
            bufsize_t end = context->reader->nonspace, last = markdown_core_reader_length(context->reader);
            while (end < last && data[end] == '#') {
                end++;
            }
            while (last > end && (data[last - 1] == '\n' || data[last - 1] == '\r')) {
                last--;
            }
            if (end == last) {
                markdown_core_line_report(self->needs[HEADING_TOPIC_LINES], parser, context->reader->nonspace, end);
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
 * long heading's children, which its seams keep, so that an edit that leaps
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

/* What the preparation and the projections keep, let go with the parse. */
static void headings_release(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    markdown_core_free(state->held);
    markdown_core_free(state->stack);
    if (state->scratch.ptr) {
        markdown_core_strbuf_free(&state->scratch);
    }
    *state = (markdown_core_heading_state){.held = NULL};
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
    .needs = HEADING_NEEDS,
    .spaces = HEADING_SPACES,
    .name = "heading",
    .state_size = sizeof(markdown_core_heading_state),
    .run_state_size = sizeof(markdown_core_heading_run),
    .finish = &HEADING_FINISH,
    .content = &HEADING_CONTENT,
    .inlines = &HEADING_INLINES,
    .opens = &HEADING_OPENS,
    .lines = &HEADING_LINES,
    .edit = &HEADING_EDIT,
    .follows = HEADING_FOLLOWS,
};

/* A heading's record (markdown_core_record_ops): level 1 until its marker
 * says otherwise; the label it declares, in the pool's storage; its name,
 * whose base it may borrow and whose shown form is its own. A view copies
 * the label, drops the shown form and borrows the base. */
static void init_heading(markdown_core_node *node) {
    node->as.heading->level = 1;
    markdown_core_strbuf_init_zeroed(&node->as.heading->name.base);
}

static void free_heading(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_heading *heading = node->as.heading;
    if (heading->label.len) {
        markdown_core_node_pool_bytes_free(pool, heading->label.data);
    }
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
    (void)node;
    markdown_core_heading *heading = copy->as.heading;
    bool copied = true;
    if (heading->label.len && (failed || !(copied = markdown_core_node_label_copy(pool, &heading->label)))) {
        heading->label = (markdown_core_chunk){0};
    }
    heading->name.shown = NULL;
    if (markdown_core_strbuf_owns(&heading->name.base)) {
        markdown_core_strbuf_borrow(&heading->name.base);
    }
    return copied;
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
