#include <string.h>
#include "services.h"
#include "cross_link.h"
#include "element.h"
#include "inlines.h"
#include "node.h"
#include "parser.h"
#include "inline_internal.h"

/* The services this element asks, as `self->needs` holds them: an embed's
 * dimensions (services.h). */
enum { CROSS_LINK_DIMENSIONS };
static const char *const CROSS_LINK_NEEDS[] = {[CROSS_LINK_DIMENSIONS] = MARKDOWN_CORE_SERVICE_DIMENSIONS, NULL};

/* The cross link whose opener runs from `start` to `body`, given up where
 * its body's reading stopped, at `stop`, kept on the run's node
 * (markdown_core_inline_open): its path, up to the first `#`, and the anchor
 * after it, which keeps a leading `^`; a body past its separator reads a
 * label, which is no slot, so the opener stands alone. */
static void S_report(markdown_core_inline_state *inline_state, bool embedded, bufsize_t start, bufsize_t body,
                     bufsize_t hash, bufsize_t label, bufsize_t stop) {
    const markdown_core_node_kind kind = embedded ? MARKDOWN_CORE_KIND_CROSS_EMBEDDED : MARKDOWN_CORE_KIND_CROSS_LINK;
    const markdown_core_field fields[2] = {{MARKDOWN_CORE_SLOT_DEST, kind, body, hash >= 0 ? hash : stop},
                                           {MARKDOWN_CORE_SLOT_ANCHOR, kind, hash + 1, stop}};
    markdown_core_inline_open(inline_state, kind, start, body, fields, label >= 0 ? 0 : hash >= 0 ? 2 : 1);
}

/* No speculative allocation and no cursor movement until recognition succeeds.
 * A candidate stops at the first bracket or line ending. Thus candidates cannot
 * nest or share a suffix: at most the ![[ and [[ attempts inspect the same body.
 * Recognition inspects each byte at most twice, including failed and unclosed
 * forms. A successful embed then inspects only its bounded numeric suffix. */
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    markdown_core_chunk *input = markdown_core_inline_state_get_chunk(inline_state);
    const unsigned char *s = input->data;
    bufsize_t start = markdown_core_inline_state_get_offset(inline_state);
    bool embedded = character == '!';
    bufsize_t opener_length = embedded ? 3 : 2;
    bufsize_t body;
    bufsize_t i, hash = -1, separator = -1, label = -1, target_end = -1;
    bufsize_t dimension_separator = -1, dimension_separator_length = 0;
    bool part_empty = false, block_id = false;
    markdown_core_node *node;
    markdown_core_cross_reference *cross;
    markdown_core_cross_link_work *counts = self->state;
    counts->scan++;
    if (input->len - start < opener_length) {
        markdown_core_inline_state_saw_end(inline_state);
        return NULL;
    }
    body = start + opener_length;
    if (s[body - 2] != '[' || s[body - 1] != '[') {
        return NULL;
    }
    for (i = body; i < input->len; i++) {
        unsigned char c = s[i];
        counts->scan++;
        if (c == '[' || c == '\n' || c == '\r') {
            S_report(inline_state, embedded, start, body, hash, label, separator >= 0 ? separator : i);
            return NULL;
        }
        if (c == ']') {
            if (i + 1 >= input->len) {
                markdown_core_inline_state_saw_end(inline_state);
            }
            if (i + 1 >= input->len || s[i + 1] != ']') {
                S_report(inline_state, embedded, start, body, hash, label, separator >= 0 ? separator : i);
                return NULL;
            }
            break;
        }
        if (label >= 0) {
            /* Labels are raw, not inline-parsed. Both authored separator forms
             * retain the cross-link grammar, including contracted table pipes. */
            if (c == '|' || (c == '\\' && i + 1 < input->len && s[i + 1] == '|')) {
                dimension_separator = i;
                dimension_separator_length = c == '|' ? 1 : 2;
                i += dimension_separator_length - 1;
                counts->scan += (size_t)dimension_separator_length - 1;
            }
            continue;
        }
        if (c == '|' || (c == '\\' && i + 1 < input->len && s[i + 1] == '|')) {
            separator = i;
            label = i + (c == '|' ? 1 : 2);
            i = label - 1;
            continue;
        }
        if (c == '#') {
            if (part_empty) {
                S_report(inline_state, embedded, start, body, hash, label, i);
                return NULL;
            }
            if (hash < 0) {
                hash = i;
                block_id = i + 1 < input->len && s[i + 1] == '^';
            } else {
                block_id = false;
            }
            part_empty = true;
        } else {
            part_empty = false;
            if (hash >= 0 && i != hash + 1 &&
                !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
                block_id = false;
            }
        }
    }
    target_end = separator >= 0 ? separator : i;
    /* A body the end cuts short may close once the input goes on. */
    if (i == input->len) {
        markdown_core_inline_state_saw_end(inline_state);
    }
    if (i == input->len || target_end == body || part_empty) {
        S_report(inline_state, embedded, start, body, hash, label, target_end);
        return NULL;
    }
    /* A lone ^ is an ordinary nonempty heading part, not a block identifier. */
    block_id = block_id && target_end > hash + 2;
    node = markdown_core_parser_make_node_with_ext(
        parser, embedded ? MARKDOWN_CORE_NODE_CROSS_EMBEDDED : MARKDOWN_CORE_NODE_CROSS_LINK, self->element);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    cross = markdown_core_node_cross_reference(node);
    cross->path = markdown_core_chunk_dup(input, body, (hash >= 0 ? hash : target_end) - body);
    if (hash >= 0) {
        bufsize_t anchor = hash + (block_id ? 2 : 1);
        cross->anchor =
            markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, anchor, target_end - anchor));
    }
    if (label >= 0) {
        cross->label = markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, label, i - label));
        bufsize_t suffix = dimension_separator >= 0 ? dimension_separator - label : 0;
        const markdown_core_need need = self->needs[CROSS_LINK_DIMENSIONS];
        const markdown_core_dimension_ops *dimensions = need.ops;
        if (embedded && dimensions &&
            dimensions->parse(need.instance, cross->label.value, suffix, dimension_separator_length,
                              &node->as.cross_embedded->dimensions.value, &counts->dimensions)) {
            node->as.cross_embedded->dimensions.has_value = true;
            cross->label.value.len = suffix;
        }
    }
    if (!markdown_core_chunk_to_cstr(&cross->path) ||
        (cross->anchor.has_value && !markdown_core_chunk_to_cstr(&cross->anchor.value)) ||
        (cross->label.has_value && !markdown_core_chunk_to_cstr(&cross->label.value))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    markdown_core_inline_state_place(inline_state, node, (int)start, (int)i + 1);
    /* Its path, its anchor, and its label and dimensions, which are no slot. */
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, node);
    markdown_core_field_spot spots[4];
    size_t count = 0;
    /* A target with no path, `[[#t]]`, holds an empty one, as one with an
     * empty fragment holds an empty anchor. */
    spots[count++] = (markdown_core_field_spot){body, hash >= 0 ? hash : target_end, MARKDOWN_CORE_SLOT_DEST, true,
                                                MARKDOWN_CORE_FIELD};
    if (hash >= 0) {
        spots[count++] = (markdown_core_field_spot){hash + (block_id ? 2 : 1), target_end, MARKDOWN_CORE_SLOT_ANCHOR,
                                                    true, MARKDOWN_CORE_FIELD};
    }
    if (label >= 0) {
        const bool sized = embedded && node->as.cross_embedded->dimensions.has_value;
        const bufsize_t end = !sized ? i : dimension_separator >= 0 ? dimension_separator : label;
        spots[count++] = (markdown_core_field_spot){label, end, MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD};
        if (sized) {
            spots[count++] = (markdown_core_field_spot){
                dimension_separator >= 0 ? dimension_separator + dimension_separator_length : label, i,
                MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD};
        }
    }
    markdown_core_field_many(&fields, spots, count);
    markdown_core_inline_state_set_offset(inline_state, i + 2);
    return node;
}

static const markdown_core_inline_ops CROSS_LINK_INLINES = {
    .match = match,
    .dispatch = "[!",
};

/* A cross link reads as its label in a name, or its path and anchor. */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    const markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
    if (cross->label.has_value) {
        return words->text(words->context, cross->label.value.data, cross->label.value.len);
    }
    return words->text(words->context, cross->path.data, cross->path.len) &&
           (!cross->anchor.has_value || words->text(words->context, cross->anchor.value.data, cross->anchor.value.len));
}

const markdown_core_facet MARKDOWN_CORE_CROSS_LINK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_CROSS_LINK, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_CROSS_EMBEDDED, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CROSS_LINK = {
    .needs = CROSS_LINK_NEEDS,
    .facets = MARKDOWN_CORE_CROSS_LINK_FACETS,
    .name = "cross_link",
    .state_size = sizeof(markdown_core_cross_link_work),
    .inlines = &CROSS_LINK_INLINES,
};

/* A cross reference's parts (markdown_core_record_ops), which view its text
 * until it settles: both cross kinds hold one, a CrossEmbedded's beside its
 * dimensions. */
static void free_reference_parts(markdown_core_cross_reference *cross) {
    markdown_core_chunk_free(&cross->path);
    markdown_core_optional_chunk_free(&cross->anchor);
    markdown_core_optional_chunk_free(&cross->label);
}

static bool own_reference_parts(markdown_core_cross_reference *cross) {
    return markdown_core_chunk_own_viewed(&cross->path) &&
           (!cross->anchor.has_value || markdown_core_chunk_own_viewed(&cross->anchor.value)) &&
           (!cross->label.has_value || markdown_core_chunk_own_viewed(&cross->label.value));
}

static void free_cross_link(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    free_reference_parts(node->as.cross_link);
}

static bool viewed_cross_link(markdown_core_node *node) { return own_reference_parts(node->as.cross_link); }

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CROSS_LINK = {
    .size = sizeof(markdown_core_cross_reference),
    .free = free_cross_link,
    .viewed = viewed_cross_link,
};

static void free_cross_embedded(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    free_reference_parts(&node->as.cross_embedded->reference);
}

static bool viewed_cross_embedded(markdown_core_node *node) {
    return own_reference_parts(&node->as.cross_embedded->reference);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CROSS_EMBEDDED = {
    .size = sizeof(markdown_core_cross_embedded),
    .free = free_cross_embedded,
    .viewed = viewed_cross_embedded,
};
