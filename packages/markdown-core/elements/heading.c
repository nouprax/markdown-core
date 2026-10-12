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
static markdown_core_key_index_slot *anchor_slot(markdown_core_parser *parser, markdown_core_heading_state *state,
                                                 markdown_core_key_index *index, markdown_core_chunk key) {
    state->anchor_work += (size_t)key.len + 1;
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(index, key.data, key.len);
    if (!slot) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return slot;
}

/* `node` declares `spelling` in its family (registry.h) with `role`: an
 * explicit anchor reserves it, and a heading with no explicit anchor takes
 * the first spelling of it as a base that its family leaves free. */
static markdown_core_fact *declare_anchor(markdown_core_parser *parser, markdown_core_node *node,
                                          markdown_core_fact_role role, const markdown_core_chunk *spelling) {
    const markdown_core_chunk stem = markdown_core_anchor_stem(spelling->data, (uint32_t)spelling->len);
    markdown_core_fact *fact =
        markdown_core_registry_declare(parser->registry, node, MARKDOWN_CORE_KEY_FAMILY, role, stem.data,
                                       (uint32_t)stem.len, spelling->data, (uint32_t)spelling->len);
    if (!fact) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    return fact;
}

/* The headings project no anchor base yet. */
void markdown_core_headings_init(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    markdown_core_strbuf_init(&state->base, 0);
}

/* Release the storage the headings' anchor bases were projected in, which
 * a parse that failed before the document began never made. */
void markdown_core_headings_dispose(const markdown_core_element_instance *self) {
    markdown_core_heading_state *state = self->state;
    if (state->base.ptr) {
        markdown_core_strbuf_free(&state->base);
    }
    markdown_core_free(state->projections.values);
    state->projections = (anchor_projection_stack){0};
}

static void project_anchor_literal(markdown_core_heading_state *state, markdown_core_strbuf *base,
                                   const unsigned char *text, bufsize_t length) {
    state->anchor_work += (size_t)length;
    markdown_core_utf8proc_anchor(base, text, length);
}

static bool push_anchor_projection(markdown_core_parser *parser, anchor_projection_stack *stack,
                                   anchor_projection projection) {
    if (projection.kind == ANCHOR_KEY ? !projection.node : !markdown_core_stem_count(projection.stem)) {
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
    stack->values[stack->count++] = projection;
    return true;
}

/* The nodes of `stem`, to project in order. */
static bool push_anchor_run(markdown_core_parser *parser, anchor_projection_stack *stack,
                            const markdown_core_stem *stem, anchor_projection_kind kind) {
    return push_anchor_projection(parser, stack, (anchor_projection){.stem = stem, .kind = kind});
}

/* The heading's anchor base, projected from its inlines and appended to
 * `base`. The stack's storage is the caller's and serves every heading of the
 * pass, as `base` does; each heading starts it empty. */
static void heading_anchor_base(markdown_core_parser *parser, markdown_core_heading_state *state,
                                markdown_core_node *heading, markdown_core_strbuf *base,
                                anchor_projection_stack *stack) {
    const bufsize_t start = base->size;
    stack->count = 0;
    push_anchor_run(parser, stack, heading->children, ANCHOR_CONTENT);
    while (stack->count && !parser->error && !base->oom) {
        /* The top level gives its next node, and is done once it has none
         * left; what that node pushes is projected before the rest. */
        anchor_projection *top = &stack->values[stack->count - 1];
        anchor_projection_kind kind = top->kind;
        markdown_core_node *node;
        if (kind == ANCHOR_KEY) {
            node = top->node;
            stack->count--;
        } else {
            node = markdown_core_stem_at(top->stem, top->index++);
            if (top->index == markdown_core_stem_count(top->stem)) {
                stack->count--;
            }
        }
        state->anchor_work++;
        if (kind == ANCHOR_KEY) {
            project_anchor_literal(state, base, (const unsigned char *)"@", 1);
            project_anchor_literal(state, base, node->as.citation->value.data, node->as.citation->value.len);
            continue;
        }
        if (kind == ANCHOR_CITATIONS) {
            markdown_core_citation_item *item = node->as.citation;
            if (item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
                push_anchor_run(parser, stack, item->suffix ? item->suffix->children : NULL, ANCHOR_CONTENT);
                push_anchor_projection(parser, stack, (anchor_projection){.node = node, .kind = ANCHOR_KEY});
                push_anchor_run(parser, stack, item->prefix ? item->prefix->children : NULL, ANCHOR_CONTENT);
            } else if (item->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
                push_anchor_projection(parser, stack, (anchor_projection){.node = node, .kind = ANCHOR_KEY});
            }
            continue;
        }
        switch (node->kind) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_CODE:
            project_anchor_literal(state, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_FORMULA: {
            const char *literal = markdown_core_elements_get_formula_literal(node);
            project_anchor_literal(state, base, (const unsigned char *)literal, (bufsize_t)strlen(literal));
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
            push_anchor_run(parser, stack, node->children, ANCHOR_CITATIONS);
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
            push_anchor_run(parser, stack, node->children, ANCHOR_CONTENT);
            break;
        case MARKDOWN_CORE_NODE_DIRECTIVE: {
            markdown_core_node *label = markdown_core_directive_label(node);
            push_anchor_run(parser, stack, label ? label->children : NULL, ANCHOR_CONTENT);
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

/* A heading's base fact, in tree order of the headings that declare them
 * (5.7). */
static int base_order(const void *left, const void *right) {
    const uint64_t a = (*(markdown_core_fact *const *)left)->node->order->label;
    const uint64_t b = (*(markdown_core_fact *const *)right)->node->order->label;
    return (a > b) - (a < b);
}

/* THE ANCHORS OF ONE FAMILY (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.7): its explicit anchors reserve their spellings, and each heading of it
 * with no explicit anchor, in source order, takes the first spelling of its
 * base that is free: the base, or the base with the next unused `-N`. A
 * heading's spelling can only meet spellings of its own family, so each
 * family is assigned alone, as one fresh pass over the document would assign
 * it. A heading the parse made takes its anchor; one shared with the old
 * tree -- one it took, or the old node one it made settled as -- gives its
 * place to a copy that takes the anchor when its anchor changes (5.7,
 * 5.11). */
static void assign_family(markdown_core_parser *parser, markdown_core_heading_state *state,
                          const markdown_core_key *family, markdown_core_fact ***bases, size_t *capacity,
                          markdown_core_strbuf *base) {
    size_t count = 0, reserved = 0;
    for (markdown_core_fact *fact = family->facts; fact; fact = fact->next) {
        if (fact->role == MARKDOWN_CORE_FACT_BASE) {
            markdown_core_fact **grown = markdown_core_reserve(*bases, capacity, count + 1, sizeof(*grown));
            if (!grown) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return;
            }
            *bases = grown;
            (*bases)[count++] = fact;
        } else {
            reserved++;
        }
    }
    if (!count) {
        return;
    }
    qsort(*bases, count, sizeof(**bases), base_order);
    markdown_core_key_index index;
    if (!markdown_core_key_index_init(&index, reserved + count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    for (markdown_core_fact *fact = family->facts; fact && !parser->error; fact = fact->next) {
        if (fact->role == MARKDOWN_CORE_FACT_RESERVE) {
            state->anchor_work++;
            markdown_core_key_index_slot *slot =
                anchor_slot(parser, state, &index, (markdown_core_chunk){fact->text, (bufsize_t)fact->length, 0});
            if (slot && !slot->key) {
                markdown_core_key_index_commit(&index, slot, fact->text);
                slot->value.counter = 1;
            }
        }
    }
    for (size_t i = 0; i < count && !parser->error; i++) {
        markdown_core_fact *fact = (*bases)[i];
        markdown_core_node *node = fact->node;
        markdown_core_chunk *anchor = &node->attributes.anchor;
        markdown_core_strbuf_clear(base);
        markdown_core_strbuf_put(base, fact->text, (bufsize_t)fact->length);
        const bufsize_t base_length = base->size;
        markdown_core_key_index_slot *entry =
            anchor_slot(parser, state, &index, (markdown_core_chunk){fact->text, (bufsize_t)fact->length, 0});
        markdown_core_key_index_slot *candidate = entry;
        if (entry && entry->key) {
            do {
                markdown_core_strbuf_truncate(base, base_length);
                append_anchor_suffix(base, entry->value.counter++);
                if (base->oom) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                /* Only a vacant candidate can grow the index. The base
                 * cursor is updated before that call and never used after
                 * it returns a vacant entry, so no pointer survives growth. */
                candidate = anchor_slot(parser, state, &index, (markdown_core_chunk){base->ptr, base->size, 0});
            } while (candidate && candidate->key);
        }
        if (parser->error || base->oom || !candidate) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            break;
        }
        if (anchor->len == base->size && !memcmp(anchor->data, base->ptr, (size_t)base->size)) {
            markdown_core_key_index_commit(&index, candidate, anchor->data);
        } else {
            if (fact->edit != parser->registry->edit || node->refs > 1) {
                markdown_core_node *copy = markdown_core_node_copy(parser->pool, node);
                if (!copy) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                if (!markdown_core_registry_move(parser->registry, node, copy)) {
                    markdown_core_parser_release_node(parser, copy);
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    break;
                }
                if (!markdown_core_parser_replace(parser, node, copy)) {
                    break;
                }
                anchor = &copy->attributes.anchor;
            }
            markdown_core_chunk_free(anchor);
            *anchor = (markdown_core_chunk){base->ptr, base->size, 0};
            if (!markdown_core_chunk_to_cstr(anchor)) {
                *anchor = (markdown_core_chunk){0};
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                break;
            }
            markdown_core_key_index_commit(&index, candidate, anchor->data);
        }
        candidate->value.counter = 1;
    }
    markdown_core_key_index_free(&index);
}

/* A node declares its anchor as it is numbered, or, a root read again, as it
 * completes (5.7): an anchor its text wrote reserves its spelling for as
 * long as the node is in the tree, and a heading whose text wrote none,
 * complete by then, declares the base its text projects to. */
void markdown_core_headings_observe(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node *node) {
    markdown_core_heading_state *state = self->state;
    const markdown_core_chunk *anchor = &node->attributes.anchor;
    if (anchor->len) {
        declare_anchor(parser, node, MARKDOWN_CORE_FACT_RESERVE, anchor);
        return;
    }
    if (node->kind != MARKDOWN_CORE_NODE_HEADING) {
        return;
    }
    markdown_core_strbuf_clear(&state->base);
    heading_anchor_base(parser, state, node, &state->base, &state->projections);
    if (!parser->error) {
        declare_anchor(parser, node, MARKDOWN_CORE_FACT_BASE,
                       &(markdown_core_chunk){state->base.ptr, state->base.size, 0});
    }
}

/* Every family a fact joined or left is assigned again. */
void markdown_core_headings_finish(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_heading_state *state = self->state;
    markdown_core_fact **bases = NULL;
    size_t capacity = 0;
    for (const markdown_core_key *key = parser->registry->marked; key && !parser->error; key = key->marked_next) {
        if (key->group == MARKDOWN_CORE_KEY_FAMILY) {
            assign_family(parser, state, key, &bases, &capacity, &state->base);
        }
    }
    markdown_core_free(bases);
}

/* Where an ATX heading's text in `data` ends before its closing sequence,
 * which follows `line`, the start of its last line, or a space, and the
 * spaces before it; `end` when it has none. */
static bufsize_t heading_closing(const unsigned char *data, bufsize_t line, bufsize_t end) {
    bufsize_t hashes = end;
    while (hashes > line && data[hashes - 1] == '#') {
        hashes--;
    }
    if (hashes == end || (hashes != line && !markdown_core_is_space_or_tab(data[hashes - 1]))) {
        return end;
    }
    while (hashes > line && markdown_core_is_space_or_tab(data[hashes - 1])) {
        hashes--;
    }
    return hashes;
}

/* THE TEXT A HEADING'S LINES HOLD (5.8): where its attribute container
 * begins in `text`, -1 when its last line does not end with one, and where
 * its text ends, `*end`: before the container and the spaces and an ATX
 * heading's closing sequence before it, and, without one, before an ATX
 * heading's closing sequence and the space before it. */
static bufsize_t heading_text(markdown_core_attribute_parser *attributes, const markdown_core_chunk *text, bool setext,
                              bufsize_t *end) {
    const unsigned char *data = text->data;
    const bufsize_t length = text->len;
    bufsize_t line = length, start = -1;
    /* Trailing attributes end the heading's last line with `}`. Only a
     * heading that does is walked back to that line's start, which is where
     * the attributes' opening brace is searched from. */
    if (length > 0 && data[length - 1] == '}') {
        while (line > 0 && !markdown_core_is_line_end(data[line - 1])) {
            line--;
        }
        start = markdown_core_attributes_tail(attributes, line, length);
    }
    if (start >= 0) {
        bufsize_t at = start;
        while (at > line && markdown_core_is_space_or_tab(data[at - 1])) {
            at--;
        }
        *end = setext ? at : heading_closing(data, line, at);
        return start;
    }
    markdown_core_chunk rest = *text;
    if (!setext && (rest.len = heading_closing(data, 0, length)) != length) {
        markdown_core_chunk_rtrim(&rest);
    }
    *end = rest.len;
    return -1;
}

/* A HEADING CLOSES on its text (5.7, 5.8). Its text is read up to its
 * trailing attribute container with no key defined: no lookup is
 * recorded, and the parse is dropped. When the read reaches the container
 * at a token's end, the heading takes the attributes it writes, and its
 * content ends where its text does; else the container is text, and only
 * an ATX heading's closing sequence is left out. When the read meets no
 * bracket a later parse could resolve, and no field, the text is writable
 * as a reference label, and the heading declares it, normalized: an
 * occurrence naming it resolves to the heading when no Reference declares
 * it. No definition changes any of this, which is why it is read before
 * the inline stage parses the content. */
static void finalize_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                             markdown_core_frame *frame) {
    (void)self;
    markdown_core_node *node = frame->node;
    markdown_core_draft *builder = markdown_core_parser_draft(parser, node, false);
    if (!builder) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, builder, &inline_state);
    markdown_core_attribute_parser attributes = {
        .data = inline_state.input.data, .length = inline_state.input.len, .scratch = &parser->attribute_scratch};
    bufsize_t end;
    const bufsize_t start = heading_text(&attributes, &inline_state.input, node->as.heading->setext, &end);
    if (start >= 0) {
        inline_state.text_end = end;
    } else {
        inline_state.input.len = end;
    }
    bool writable = true, claimed = false;
    while (!parser->error && !inline_state.error && !attributes.oom) {
        if (inline_state.pos == inline_state.text_end) {
            bufsize_t past;
            claimed = markdown_core_attributes_parse(&attributes, start, &node->attributes, &past);
            break;
        }
        unsigned char c = markdown_core_inline_peek_char(&inline_state);
        /* Attribute ownership and opaque tokens are decided by the same
         * cursor as every inline. A live bracket or a field makes the text
         * unwritable as a reference label. */
        if ((inline_state.last_delim && inline_state.last_delim->kind == DELIMITER_FIELD) ||
            (inline_state.pos < inline_state.input.len && inline_state.pos >= inline_state.opaque_end &&
             (c == '[' || c == ']' ||
              ((c == '!' || c == '^') && markdown_core_inline_peek_char_n(&inline_state, 1) == '[')))) {
            writable = false;
            if (start < 0) {
                break;
            }
        }
        if (markdown_core_inline_is_eof(&inline_state) || !markdown_core_inline_parse_inline(parser, &inline_state)) {
            break;
        }
    }
    if (attributes.oom) {
        inline_state.error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    const bufsize_t length = claimed ? end : start >= 0 ? inline_state.input.len : end;
    markdown_core_attribute_parser_free(&attributes);
    markdown_core_inline_clear_inlines(&inline_state);
    markdown_core_drafts_clear(&parser->drafts, parser->pool);
    if (parser->error) {
        return;
    }
    markdown_core_strbuf_truncate(&node->content, length);
    markdown_core_chunk label = {node->content.ptr, length, 0};
    if (writable && label.len > 0 && label.len <= MAX_LINK_LABEL_LENGTH &&
        markdown_core_inline_reference_label_length(label.data, label.len) == label.len) {
        markdown_core_chunk *declared = &node->as.heading->label;
        if (!markdown_core_parser_normalize_label(parser, &label, declared)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        } else {
            markdown_core_parser_declare(parser, node, MARKDOWN_CORE_KEY_REFERENCE, declared);
        }
    }
}

#define peek_at(input, at) ((input)->data[(at)])
static bool open_atx(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_frame **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    bufsize_t hashpos;
    int level = 0;
    bufsize_t heading_startpos = parser->input.first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->input.first_nonspace + matched - parser->input.offset,
                                       false);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
    if (!*container) {
        return false;
    }

    hashpos = markdown_core_chunk_strchr(input, '#', parser->input.first_nonspace);

    while (peek_at(input, hashpos) == '#') {
        level++;
        hashpos++;
    }

    (*container)->node->as.heading->level = level;
    (*container)->node->as.heading->setext = false;
    (*container)->node->internal_offset = matched;

    return true;
}
static bool open_setext(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_frame **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    bool has_content;
    // markdown_core_block_finalize paragraph, resolving reference links
    has_content = markdown_core_block_resolve_reference_link_definitions(parser, *container);

    if (has_content) {

        markdown_core_node_set_kind_result result =
            markdown_core_parser_set_node_kind(parser, *container, MARKDOWN_CORE_NODE_HEADING);
        if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
            if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            return false;
        }
        (*container)->node->as.heading->level = matched;
        (*container)->node->as.heading->setext = true;
        markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->input.offset, false);
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
                            unsigned char *data, int length, markdown_core_frame *container) {
    return 0;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .peers = HEADING_PEERS,

    .name = "heading",
    .maximum_block_indent = 3,
    .scan_block_start = scan_heading,
    /* ATX `#`; a setext underline of `=` or `-`. */
    .scan_block_gate = {.bytes = "#=-"},
    .last_block_matches = continue_heading,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inline_content = true,
    .finalize_block = finalize_heading,
    .blank_opaque = true,
    .state_size = sizeof(markdown_core_heading_state),
};
