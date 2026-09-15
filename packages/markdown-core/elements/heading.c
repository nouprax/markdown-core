#include "heading_scanners.h"
#include "citation.h"
#include "heading.h"
#include "link.h"
#include "directive.h"
#include "inline_internal.h"
#include "block_internal.h"
typedef enum { ANCHOR_CONTENT, ANCHOR_CITATIONS, ANCHOR_KEY } anchor_projection_kind;
typedef struct {
    markdown_core_node *node;
    anchor_projection_kind kind;
} anchor_projection;

typedef struct {
    anchor_projection *values;
    size_t count, capacity;
} anchor_projection_stack;
void markdown_core_block_register_heading(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_heading_collection *headings = &parser->headings;
    if (headings->count == headings->capacity) {
        size_t capacity = headings->capacity ? headings->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*headings->values)) {
            parser->oom = true;
            return;
        }
        void *values = parser->mem->realloc(headings->values, capacity * sizeof(*headings->values));
        if (!values) {
            parser->oom = true;
            return;
        }
        headings->values = values;
        headings->capacity = capacity;
    }
    headings->values[headings->count++] = (markdown_core_heading_parse){.node = node};
}

static markdown_core_key_index_slot *anchor_slot(markdown_core_parser *parser, anchor_registry *registry,
                                                 markdown_core_chunk key) {
    parser->anchor_work += (size_t)key.len + 1;
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&registry->index, key.data, key.len);
    if (!slot) {
        parser->oom = true;
    }
    return slot;
}

void markdown_core_block_reserve_node_anchor(markdown_core_parser *parser, anchor_registry *registry,
                                             markdown_core_node *node) {
    const markdown_core_chunk *anchor = markdown_core_node_anchor_chunk(node);
    if (!anchor->len) {
        return;
    }
    parser->anchor_work++;
    if (anchor != &node->attributes.anchor) {
        const unsigned char *identity = (const unsigned char *)&node->as.link->resource;
        void *existing = NULL;
        if (!markdown_core_key_index_insert(&registry->resources, identity, sizeof(node->as.link->resource),
                                            node->as.link->resource, 0, &existing)) {
            parser->oom = true;
            return;
        }
        if (existing) {
            return;
        }
    }
    markdown_core_key_index_slot *slot = anchor_slot(parser, registry, *anchor);
    if (slot && !slot->key) {
        markdown_core_key_index_commit(&registry->index, slot, anchor->data);
        slot->value.counter = 1;
    }
}

void markdown_core_block_prepare_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings) {
    if (!markdown_core_order_source_entries(parser->mem, headings->values, headings->count, sizeof(*headings->values),
                                            markdown_core_source_key)) {
        parser->oom = true;
        return;
    }
    /* The reference map compares explicitness and original source positions,
     * independently of mapped-input scheduling and declaration closure order. */
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_prepare_heading(parser, &headings->values[i]);
        if (parser->refmap->oom) {
            parser->oom = true;
        }
    }
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_finish_heading(parser, &headings->values[i]);
    }
}

void markdown_core_block_dispose_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings) {
    for (size_t i = 0; i < headings->count; i++) {
        markdown_core_dispose_heading(&headings->values[i]);
    }
    parser->mem->free(headings->values);
    *headings = (markdown_core_heading_collection){0};
}

static void project_anchor_literal(markdown_core_parser *parser, markdown_core_strbuf *base, const unsigned char *text,
                                   bufsize_t length) {
    parser->anchor_work += (size_t)length;
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
            parser->oom = true;
            return false;
        }
        void *values = parser->mem->realloc(stack->values, capacity * sizeof(*stack->values));
        if (!values) {
            parser->oom = true;
            return false;
        }
        stack->values = values;
        stack->capacity = capacity;
    }
    stack->values[stack->count++] = (anchor_projection){node, kind};
    return true;
}

static void heading_anchor_base(markdown_core_parser *parser, markdown_core_node *heading, markdown_core_strbuf *base) {
    anchor_projection_stack stack = {0};
    push_anchor_projection(parser, &stack, heading->first_child, ANCHOR_CONTENT);
    while (stack.count && !parser->oom && !base->oom) {
        anchor_projection projection = stack.values[--stack.count];
        markdown_core_node *node = projection.node;
        parser->anchor_work++;
        if (projection.kind == ANCHOR_KEY) {
            project_anchor_literal(parser, base, (const unsigned char *)"@", 1);
            project_anchor_literal(parser, base, node->as.citation->value.data, node->as.citation->value.len);
            continue;
        }
        push_anchor_projection(parser, &stack, node->next, projection.kind);
        if (projection.kind == ANCHOR_CITATIONS) {
            markdown_core_citation_item *item = node->as.citation;
            if (item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
                push_anchor_projection(parser, &stack, item->suffix ? item->suffix->first_child : NULL, ANCHOR_CONTENT);
                push_anchor_projection(parser, &stack, node, ANCHOR_KEY);
                push_anchor_projection(parser, &stack, item->prefix ? item->prefix->first_child : NULL, ANCHOR_CONTENT);
            } else if (item->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
                push_anchor_projection(parser, &stack, node, ANCHOR_KEY);
            }
            continue;
        }
        switch (node->kind) {
        case MARKDOWN_CORE_NODE_TEXT:
        case MARKDOWN_CORE_NODE_CODE:
            project_anchor_literal(parser, base, node->as.literal->data, node->as.literal->len);
            break;
        case MARKDOWN_CORE_NODE_FORMULA: {
            const char *literal = markdown_core_elements_get_formula_literal(node);
            project_anchor_literal(parser, base, (const unsigned char *)literal, (bufsize_t)strlen(literal));
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
                project_anchor_literal(parser, base, cross->label.value.data, cross->label.value.len);
            } else {
                project_anchor_literal(parser, base, cross->path.data, cross->path.len);
                if (cross->anchor.has_value) {
                    project_anchor_literal(parser, base, cross->anchor.value.data, cross->anchor.value.len);
                }
            }
            break;
        }
        case MARKDOWN_CORE_NODE_CITE:
            push_anchor_projection(parser, &stack, node->as.cite->citations, ANCHOR_CITATIONS);
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
            push_anchor_projection(parser, &stack, node->first_child, ANCHOR_CONTENT);
            break;
        case MARKDOWN_CORE_NODE_DIRECTIVE: {
            markdown_core_node *label = markdown_core_directive_label(node);
            push_anchor_projection(parser, &stack, label ? label->first_child : NULL, ANCHOR_CONTENT);
            break;
        }
        default:
            /* HTML, comments and other opaque values contribute no text. */
            break;
        }
    }
    parser->mem->free(stack.values);
    if (!base->size) {
        markdown_core_strbuf_puts(base, "section");
    }
    if (base->oom) {
        parser->oom = true;
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

void markdown_core_block_finalize_heading_anchors(markdown_core_parser *parser,
                                                  markdown_core_heading_collection *headings,
                                                  anchor_registry *registry) {
    markdown_core_strbuf base = MARKDOWN_CORE_BUF_INIT(parser->mem);
    for (size_t i = 0; i < headings->count && !parser->oom; i++) {
        markdown_core_heading_parse *heading = &headings->values[i];
        markdown_core_chunk *anchor = &heading->node->attributes.anchor;
        if (!anchor->len) {
            markdown_core_strbuf_clear(&base);
            heading_anchor_base(parser, heading->node, &base);
            if (parser->oom) {
                break;
            }
            bufsize_t base_length = base.size;
            markdown_core_key_index_slot *entry =
                anchor_slot(parser, registry, (markdown_core_chunk){base.ptr, base.size, 0});
            markdown_core_key_index_slot *candidate = entry;
            if (entry && entry->key) {
                do {
                    markdown_core_strbuf_truncate(&base, base_length);
                    append_anchor_suffix(&base, entry->value.counter++);
                    if (base.oom) {
                        parser->oom = true;
                        break;
                    }
                    /* Only a vacant candidate can grow the index. The base
                     * cursor is updated before that call and never used after
                     * it returns a vacant entry, so no pointer survives growth. */
                    candidate = anchor_slot(parser, registry, (markdown_core_chunk){base.ptr, base.size, 0});
                } while (candidate && candidate->key);
            }
            if (parser->oom) {
                break;
            }
            markdown_core_chunk_free(parser->mem, anchor);
            *anchor = (markdown_core_chunk){base.ptr, base.size, 0};
            if (!markdown_core_chunk_to_cstr(parser->mem, anchor)) {
                parser->oom = true;
                break;
            }
            markdown_core_key_index_commit(&registry->index, candidate, anchor->data);
            candidate->value.counter = 1;
        }
        markdown_core_resource *resource = heading->resource;
        if (resource && !parser->oom) {
            markdown_core_strbuf_clear(&base);
            markdown_core_strbuf_putc(&base, '#');
            markdown_core_strbuf_put(&base, anchor->data, anchor->len);
            if (base.oom) {
                parser->oom = true;
                break;
            }
            markdown_core_chunk_free(parser->mem, &resource->url);
            resource->url = (markdown_core_chunk){base.ptr, base.size, 0};
            if (!markdown_core_chunk_to_cstr(parser->mem, &resource->url)) {
                parser->oom = true;
            }
        }
    }
    markdown_core_strbuf_free(&base);
}

void markdown_core_prepare_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, heading->node, parser->refmap, &inline_state);
    while (!parser->oom && !inline_state.oom) {
        unsigned char c = markdown_core_inline_peek_char(&inline_state);
        /* Attribute ownership and opaque tokens are decided by the same
         * cursor as every inline. A live bracket makes this declaration
         * unwritable as a reference label; only its remaining inlines depend
         * on the document's completed symbol table. */
        if ((inline_state.last_delim && inline_state.last_delim->kind == DELIMITER_FIELD) ||
            (inline_state.pos != inline_state.text_end && inline_state.pos >= inline_state.opaque_end &&
             (c == '[' || c == ']' ||
              ((c == '!' || c == '^') && markdown_core_inline_peek_char_n(&inline_state, 1) == '[')))) {
            heading->pending = parser->mem->calloc(1, sizeof(inline_state));
            if (heading->pending) {
                *heading->pending = inline_state;
                return;
            }
            inline_state.oom = 1;
            break;
        }
        if (markdown_core_inline_is_eof(&inline_state) ||
            !markdown_core_inline_parse_inline(parser, &inline_state, heading->node)) {
            break;
        }
    }
    if (!parser->oom && !inline_state.oom) {
        markdown_core_inline_finish_citation_tokens(&inline_state, &inline_state.citations);
        markdown_core_inline_process_delimiters(parser, &inline_state, 0, NULL);
        markdown_core_chunk label = {inline_state.input.data, inline_state.heading_label_end, 0};
        if (label.len > 0 && label.len <= MAX_LINK_LABEL_LENGTH &&
            markdown_core_inline_reference_label_length(label.data, label.len) == label.len) {
            markdown_core_resource *resource = markdown_core_resource_new(parser->mem, markdown_core_chunk_literal(""),
                                                                          markdown_core_optional_chunk_absent());
            if (!resource) {
                inline_state.oom = 1;
            } else {
                markdown_core_map_record *record =
                    markdown_core_reference_create(parser->mem, parser->refmap, &label, resource);
                if (record) {
                    record->implicit = true;
                    record->source_key =
                        ((uint64_t)(uint32_t)heading->node->start_line << 32) | (uint32_t)heading->node->start_column;
                }
                heading->resource = record ? record->resource : NULL;
            }
        }
    }
    markdown_core_inline_clear_inlines(&inline_state);
}

void markdown_core_finish_heading(markdown_core_parser *parser, markdown_core_heading_parse *heading) {
    if (heading->pending) {
        markdown_core_inline_finish_inlines(parser, heading->pending);
        parser->mem->free(heading->pending);
        heading->pending = NULL;
    }
}

void markdown_core_dispose_heading(markdown_core_heading_parse *heading) {
    if (heading->pending) {
        markdown_core_mem *mem = heading->pending->mem;
        markdown_core_inline_clear_inlines(heading->pending);
        mem->free(heading->pending);
        heading->pending = NULL;
    }
}

void markdown_core_heading_begin_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         markdown_core_node *parent) {
    if (parent->kind == MARKDOWN_CORE_NODE_HEADING) {
        bufsize_t line = inline_state->input.len;
        while (line > 0 && !markdown_core_is_line_end(inline_state->input.data[line - 1])) {
            line--;
        }
        inline_state->attributes = (markdown_core_attribute_parser){
            .mem = parser->mem, .data = inline_state->input.data, .length = inline_state->input.len};
        inline_state->heading_attributes_start =
            markdown_core_attributes_tail(&inline_state->attributes, line, inline_state->input.len);
        if (inline_state->heading_attributes_start >= 0) {
            bufsize_t end = inline_state->heading_attributes_start;
            while (end > line &&
                   (inline_state->input.data[end - 1] == ' ' || inline_state->input.data[end - 1] == '\t')) {
                end--;
            }
            if (!parent->as.heading->setext) {
                bufsize_t hashes = end;
                while (hashes > line && inline_state->input.data[hashes - 1] == '#') {
                    hashes--;
                }
                if (hashes < end && (hashes == line || (inline_state->input.data[hashes - 1] == ' ' ||
                                                        inline_state->input.data[hashes - 1] == '\t'))) {
                    end = hashes;
                    while (end > line &&
                           (inline_state->input.data[end - 1] == ' ' || inline_state->input.data[end - 1] == '\t')) {
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
            if (hashes < inline_state->input.len && (hashes == 0 || (inline_state->input.data[hashes - 1] == ' ' ||
                                                                     inline_state->input.data[hashes - 1] == '\t'))) {
                inline_state->input.len = hashes;
                markdown_core_chunk_rtrim(&inline_state->input);
            }
        }
        if (inline_state->attributes.oom) {
            inline_state->oom = 1;
        }
    }

    inline_state->heading_label_end = inline_state->input.len;
}

bool markdown_core_heading_claim_tail(markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    if (inline_state->pos == inline_state->text_end) {
        bufsize_t end;
        if (markdown_core_attributes_parse(&inline_state->attributes, inline_state->heading_attributes_start,
                                           &parent->attributes, &end)) {
            inline_state->heading_label_end = inline_state->text_end;
            inline_state->pos = inline_state->input.len;
        }
        if (inline_state->attributes.oom) {
            inline_state->oom = 1;
        }
        return true;
    }
    return false;
}

#define peek_at(input, at) ((input)->data[(at)])
static bool open_atx(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                     block_start *start) {
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
static bool open_setext(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                        block_start *start) {
    bufsize_t matched = start->matched;
    bool has_content;
    // markdown_core_block_finalize paragraph, resolving reference links
    has_content = markdown_core_block_resolve_reference_link_definitions(parser, *container);

    if (has_content) {

        markdown_core_node_set_kind_result result = markdown_core_node_set_kind(*container, MARKDOWN_CORE_NODE_HEADING);
        if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
            if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
                parser->oom = true;
            }
            return false;
        }
        (*container)->as.heading->level = matched;
        (*container)->as.heading->setext = true;
        markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    }

    return true;
}
static bool scan_heading(markdown_core_parser *parser, block_start_context *context, block_start *start) {
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
static int continue_heading(const markdown_core_element *self, markdown_core_parser *parser, unsigned char *data,
                            int length, markdown_core_node *container) {
    return 0;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .begin_inline = markdown_core_heading_begin_inlines,
    .claim_inline_tail = markdown_core_heading_claim_tail,

    .name = "heading",
    .maximum_block_indent = 3,
    .scan_block_start = scan_heading,
    .last_block_matches = continue_heading,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inline_content = true,
    .deferred_inlines = true,
    .finalize_block = markdown_core_block_register_heading,
    .blank_opaque = true,
};
