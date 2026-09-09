#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "node.h"
#include "references.h"
#include "extension.h"
#include "../extensions/markdown-core-extensions.h"

static void S_node_unlink(markdown_core_node *node);

#define NODE_MEM(node) markdown_core_node_mem(node)

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    if (child_type == MARKDOWN_CORE_NODE_DOCUMENT) {
        return false;
    }

    if (node->extension && node->extension->can_contain_func) {
        return node->extension->can_contain_func(node->extension, node, child_type) != 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_CALLOUT:
    case MARKDOWN_CORE_NODE_SPECIMEN:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_FOOTNOTE:
        return (MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM) ||
               MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    case MARKDOWN_CORE_NODE_LIST:
        return child_type == MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_EMPHASIS:
    case MARKDOWN_CORE_NODE_STRONG:
    case MARKDOWN_CORE_NODE_MARK:
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_MEDIA:
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    default:
        break;
    }

    return false;
}

static bool S_can_contain(markdown_core_node *node, markdown_core_node *child) {
    if (node == NULL || child == NULL) {
        return false;
    }
    if (NODE_MEM(node) != NODE_MEM(child)) {
        return 0;
    }
    /* `child` must not be `node` and must not be one of its ancestors.
     *
     * This used to sit behind `markdown_core_enable_safety_checks`, a
     * process-global flag that defaulted to OFF and that only the test suite
     * ever set -- so the shipped library answered `append_child(q, q)` with
     * SUCCESS and left `q->parent == q`, and a two-node cycle took two calls.
     * Measured before it was made unconditional, with the flag in its shipped
     * position:
     *
     *     append_child(q, q)   returned 1, parent == self
     *     prepend_child(r, r)  returned 1, parent == self
     *     append_child(a, b) then append_child(b, a)  ->  a->parent == b
     *
     * A library that makes a cycle on request while its own tests deny it is
     * not testing the library. The walk is O(depth) per link and the parse's
     * depth is the document's nesting; §4.14.3b has the cost. */
    {
        markdown_core_node *cur = node;
        do {
            if (cur == child) {
                return false;
            }
            cur = cur->parent;
        } while (cur != NULL);
    }

    return markdown_core_node_can_contain_type(node, (markdown_core_node_type)child->kind);
}

/* A C99 allocation header aligns both the node and the trailing record for
 * ordinary scalar fields. The node is its first member, so its address remains
 * the address passed to free. This is allocation layout, not a field format. */
typedef union {
    markdown_core_node node;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_node_allocation;

static void *S_initial_payload(markdown_core_node *node) { return (markdown_core_node_allocation *)node + 1; }

/* Record size is a property of the kind, independent of the input shape. */
static size_t S_node_payload_size(markdown_core_node_type type) {
    size_t size = 0;
    switch ((uint16_t)type) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
        size = sizeof(markdown_core_document_value);
        break;
    case MARKDOWN_CORE_NODE_LIST:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        size = sizeof(markdown_core_list);
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        size = sizeof(markdown_core_code);
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        size = sizeof(markdown_core_html_block);
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        size = sizeof(markdown_core_heading);
        break;
    case MARKDOWN_CORE_NODE_CALLOUT:
        size = sizeof(markdown_core_callout);
        break;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        size = sizeof(markdown_core_chunk);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_MEDIA:
        size = sizeof(markdown_core_link);
        break;
    case MARKDOWN_CORE_NODE_CROSS_LINK:
        size = sizeof(markdown_core_cross_reference);
        break;
    case MARKDOWN_CORE_NODE_CROSS_EMBEDDED:
        size = sizeof(markdown_core_cross_embedded);
        break;
    case MARKDOWN_CORE_NODE_CITE:
        size = sizeof(markdown_core_cite);
        break;
    case MARKDOWN_CORE_NODE_CITATION:
        size = sizeof(markdown_core_citation_item);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        size = sizeof(markdown_core_footnote_value);
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        size = sizeof(markdown_core_specimen_value);
        break;
    case MARKDOWN_CORE_NODE_TABLE_CELL:
        size = sizeof(markdown_core_table_cell);
        break;
    default:
        break;
    }
    return size;
}

/* Establish defaults over zero-initialized storage. */
static void S_init_node_as(markdown_core_node_type type, markdown_core_node_data *as) {
    switch ((uint16_t)type) {
    case MARKDOWN_CORE_NODE_HEADING:
        as->heading->level = 1;
        break;
    case MARKDOWN_CORE_NODE_LIST:
        as->list->list_type = MARKDOWN_CORE_BULLET_LIST;
        break;
    default:
        break;
    }
}

markdown_core_node *markdown_core_node_new_with_mem_and_ext(markdown_core_node_type type, markdown_core_mem *mem,
                                                            const markdown_core_extension *extension) {
    /* Construction gives the node and its record one aligned allocation. */
    size_t payload_size = S_node_payload_size(type);
    markdown_core_node *node =
        (markdown_core_node *)mem->calloc(1, sizeof(markdown_core_node_allocation) + payload_size);
    if (!node) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &node->content, 0);
    node->kind = (uint16_t)type;
    node->extension = extension;
    node->as.data = payload_size ? S_initial_payload(node) : NULL;
    S_init_node_as(type, &node->as);

    if (node->extension && node->extension->opaque_alloc_func) {
        node->extension->opaque_alloc_func(node->extension, mem, node);
    }

    return node;
}

markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type,
                                                    const markdown_core_extension *extension) {
    return markdown_core_node_new_with_mem_and_ext(type, markdown_core_get_default_mem_allocator(), extension);
}

markdown_core_node *markdown_core_node_new_with_mem(markdown_core_node_type type, markdown_core_mem *mem) {
    return markdown_core_node_new_with_mem_and_ext(type, mem, NULL);
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_new_with_ext(type, NULL);
}

static void free_node_as(markdown_core_node *node) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.callout->variant);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT: {
        markdown_core_metadata_free(NODE_MEM(node), node->as.document->metadata);
        node->as.document->metadata = NULL;
        break;
    }
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.list->task_marker);
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.code->info);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.code->literal);
        break;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        markdown_core_chunk_free(NODE_MEM(node), node->as.literal);
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.html_block->literal);
        break;
    case MARKDOWN_CORE_NODE_CROSS_LINK:
    case MARKDOWN_CORE_NODE_CROSS_EMBEDDED: {
        markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
        markdown_core_chunk_free(NODE_MEM(node), &cross->path);
        markdown_core_optional_chunk_free(NODE_MEM(node), &cross->anchor);
        markdown_core_optional_chunk_free(NODE_MEM(node), &cross->label);
        break;
    }
    case MARKDOWN_CORE_NODE_CITATION:
        /* The affix chains are freed by the walk in `S_free_nodes`, spliced
         * in beside the children; only the referent's bytes are the arm's. */
        markdown_core_chunk_free(NODE_MEM(node), &node->as.citation->value);
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.specimen->id);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.footnote->id);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_MEDIA:
        /* One holder fewer; a resource shared with other occurrences, or
         * still held by the reference map, stays. */
        markdown_core_resource_release(NODE_MEM(node), node->as.link->resource);
        node->as.link->resource = NULL;
        break;
    default:
        break;
    }
    /* Free only the allocation this node owns separately. Pointer equality
     * cannot establish ownership: an allocator may place a replacement right
     * after a fieldless node's allocation. */
    NODE_MEM(node)->free(node->node_data_allocation);
    node->node_data_allocation = NULL;
    node->as.data = NULL;
}

// Free a markdown_core_node list and any children.
/* Splices `first`'s sibling chain into the free walk right after `e`, so the
 * walk frees it as it frees children: without recursion. */
static void S_splice_after(markdown_core_node *e, markdown_core_node *first) {
    markdown_core_node *last;
    if (first == NULL) {
        return;
    }
    last = first;
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = e->next;
    e->next = first;
}

/* The node-valued fields join the same iterative free walk as content.
 * Kind conversion uses a separate walk so its siblings remain untouched. */
static void S_splice_owned_fields(markdown_core_node *owner, markdown_core_node *after) {
    switch (owner->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        S_splice_after(after, owner->as.callout->title);
        break;
    case MARKDOWN_CORE_NODE_CITE:
        S_splice_after(after, owner->as.cite->citations);
        break;
    case MARKDOWN_CORE_NODE_CITATION:
        S_splice_after(after, owner->as.citation->suffix);
        S_splice_after(after, owner->as.citation->prefix);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT:
        S_splice_after(after, owner->as.document->footnotes);
        S_splice_after(after, owner->as.document->specimens);
        break;
    default:
        break;
    }
}

static void S_free_nodes(markdown_core_node *e) {
    markdown_core_node *next;
    while (e != NULL) {
        markdown_core_attributes_free(NODE_MEM(e), &e->attributes);
        markdown_core_strbuf_free(&e->content);

        if (e->user_data && e->user_data_free_func) {
            e->user_data_free_func(NODE_MEM(e), e->user_data);
        }

        if (e->opaque && e->extension && e->extension->opaque_free_func) {
            e->extension->opaque_free_func(e->extension, NODE_MEM(e), e);
        }

        S_splice_owned_fields(e, e);
        free_node_as(e);

        if (e->last_child) {
            // Splice children into list
            e->last_child->next = e->next;
            e->next = e->first_child;
        }
        next = e->next;
        NODE_MEM(e)->free(e);
        e = next;
    }
}

void markdown_core_node_free(markdown_core_node *node) {
    S_node_unlink(node);
    node->next = NULL;
    S_free_nodes(node);
}

markdown_core_node_type markdown_core_node_get_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NODE_NONE;
    } else {
        return (markdown_core_node_type)node->kind;
    }
}

markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node_type kind) {
    markdown_core_node_type initial_kind = (markdown_core_node_type)node->kind;
    if (kind == initial_kind) {
        return MARKDOWN_CORE_NODE_SET_KIND_OK;
    }
    node->kind = (uint16_t)kind;
    bool allowed = S_can_contain(node->parent, node);
    node->kind = (uint16_t)initial_kind;
    if (!allowed) {
        return MARKDOWN_CORE_NODE_SET_KIND_REJECTED;
    }

    /* Allocate before releasing anything. A failed conversion preserves the
     * old kind, data, and owned subtrees, with stable node identity. */
    size_t size = S_node_payload_size(kind);
    markdown_core_node_data replacement = {.data = size ? NODE_MEM(node)->calloc(1, size) : NULL};
    if (size && !replacement.data) {
        return MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED;
    }
    S_init_node_as(kind, &replacement);
    markdown_core_node fields = {0};
    S_splice_owned_fields(node, &fields);
    S_free_nodes(fields.next);
    free_node_as(node);
    node->as = replacement;
    node->node_data_allocation = replacement.data;
    node->kind = (uint16_t)kind;
    return MARKDOWN_CORE_NODE_SET_KIND_OK;
}

const char *markdown_core_node_get_type_string(markdown_core_node *node) {
    if (node == NULL) {
        return "NONE";
    }

    if (node->extension && node->extension->get_type_string_func) {
        return node->extension->get_type_string_func(node->extension, node);
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_NONE:
        return "none";
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return "document";
    case MARKDOWN_CORE_NODE_CALLOUT:
        return "callout";
    case MARKDOWN_CORE_NODE_LIST:
        return "list";
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return "list_item";
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return "code_block";
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return "html_block";
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return "comment_block";
    case MARKDOWN_CORE_NODE_PARAGRAPH:
        return "paragraph";
    case MARKDOWN_CORE_NODE_HEADING:
        return "heading";
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return "thematic_break";
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        return "footnote";
    case MARKDOWN_CORE_NODE_SPECIMEN:
        return "specimen";
    case MARKDOWN_CORE_NODE_TEXT:
        return "text";
    case MARKDOWN_CORE_NODE_SOFT_BREAK:
        return "soft_break";
    case MARKDOWN_CORE_NODE_LINE_BREAK:
        return "line_break";
    case MARKDOWN_CORE_NODE_CODE:
        return "code";
    case MARKDOWN_CORE_NODE_HTML:
        return "html";
    case MARKDOWN_CORE_NODE_COMMENT:
        return "comment";
    case MARKDOWN_CORE_NODE_EMPHASIS:
        return "emphasis";
    case MARKDOWN_CORE_NODE_STRONG:
        return "strong";
    case MARKDOWN_CORE_NODE_MARK:
        return "mark";
    case MARKDOWN_CORE_NODE_LINK:
        return "link";
    case MARKDOWN_CORE_NODE_MEDIA:
        return "media";
    case MARKDOWN_CORE_NODE_CITE:
        return "cite";
    case MARKDOWN_CORE_NODE_CITATION:
        return "citation";
    }

    return "<unknown>";
}

markdown_core_node *markdown_core_node_next(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->next;
    }
}

markdown_core_node *markdown_core_node_previous(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->prev;
    }
}

markdown_core_node *markdown_core_node_parent(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->parent;
    }
}

markdown_core_node *markdown_core_node_first_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->first_child;
    }
}

markdown_core_node *markdown_core_node_last_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->last_child;
    }
}

void *markdown_core_node_get_user_data(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->user_data;
    }
}

int markdown_core_node_set_user_data(markdown_core_node *node, void *user_data) {
    if (node == NULL) {
        return 0;
    }
    node->user_data = user_data;
    return 1;
}

int markdown_core_node_set_user_data_free_func(markdown_core_node *node, markdown_core_free_func free_func) {
    if (node == NULL) {
        return 0;
    }
    node->user_data_free_func = free_func;
    return 1;
}

const char *markdown_core_node_get_literal(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.html_block->literal);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), node->as.literal);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code->literal);

    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_literal(markdown_core_node *node, const char *content) {
    if (node == NULL) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.html_block->literal, content);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), node->as.literal, content);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code->literal, content);

    default:
        break;
    }

    return 0;
}

const char *markdown_core_node_get_string_content(markdown_core_node *node) { return (char *)node->content.ptr; }

int markdown_core_node_set_string_content(markdown_core_node *node, const char *content) {
    markdown_core_strbuf_sets(&node->content, content);
    return true;
}

int markdown_core_node_get_heading_level(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HEADING:
        return node->as.heading->level;

    default:
        break;
    }

    return 0;
}

int markdown_core_node_set_heading_level(markdown_core_node *node, int level) {
    if (node == NULL || level < 1 || level > 6) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading->level = level;
        return 1;

    default:
        break;
    }

    return 0;
}

markdown_core_list_type markdown_core_node_get_list_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_LIST;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->list_type;
    } else {
        return MARKDOWN_CORE_NO_LIST;
    }
}

int markdown_core_node_set_list_type(markdown_core_node *node, markdown_core_list_type type) {
    if (!(type == MARKDOWN_CORE_BULLET_LIST || type == MARKDOWN_CORE_ORDERED_LIST)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->list_type = type;
        return 1;
    } else {
        return 0;
    }
}

markdown_core_delim_type markdown_core_node_get_list_delim(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_DELIM;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->delimiter;
    } else {
        return MARKDOWN_CORE_NO_DELIM;
    }
}

int markdown_core_node_set_list_delim(markdown_core_node *node, markdown_core_delim_type delim) {
    if (!(delim == MARKDOWN_CORE_PERIOD_DELIM || delim == MARKDOWN_CORE_PAREN_DELIM)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->delimiter = delim;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_start(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_start(markdown_core_node *node, int start) {
    if (node == NULL || start < 0) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->start = start;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_tight(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->tight;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_tight(markdown_core_node *node, int tight) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->tight = tight == 1;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_item_index(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return node->as.list->start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_item_index(markdown_core_node *node, int idx) {
    if (node == NULL || idx < 0) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        node->as.list->start = idx;
        return 1;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_fence_info(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* ABSENT IS NULL. `markdown_core_chunk_to_cstr` allocates a `""` for a
         * chunk with no data, which would answer "the source wrote an empty
         * info string" for a fence that wrote none (requirement 14). */
        if (!node->as.code->info.has_value) {
            return NULL;
        }
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code->info.value);
    } else {
        return NULL;
    }
}

int markdown_core_node_set_fence_info(markdown_core_node *node, const char *info) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* A NULL argument is ABSENCE and anything else is presence, including
         * `""`; the caller states which, and this is the write site. */
        if (!markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code->info.value, info)) {
            return 0;
        }
        node->as.code->info.has_value = info != NULL;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_fence_closed(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return node->as.code->fenced && node->as.code->fence_closed;
    } else {
        return 0;
    }
}

int markdown_core_node_get_fenced(markdown_core_node *node, int *length, int *offset, char *character) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        *length = node->as.code->fence_length;
        *offset = node->as.code->fence_offset;
        *character = node->as.code->fence_char;
        return node->as.code->fenced;
    } else {
        return 0;
    }
}

int markdown_core_node_set_fenced(markdown_core_node *node, int fenced, int length, int offset, char character) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        node->as.code->fenced = (int8_t)fenced;
        node->as.code->fence_length = (uint8_t)length;
        node->as.code->fence_offset = (uint8_t)offset;
        node->as.code->fence_char = character;
        return 1;
    } else {
        return 0;
    }
}

markdown_core_resource *markdown_core_resource_new(markdown_core_mem *mem, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title) {
    markdown_core_resource *resource = (markdown_core_resource *)mem->calloc(1, sizeof(*resource));
    if (!resource) {
        return NULL;
    }
    resource->url = url;
    resource->title = title;
    resource->holders = 1;
    return resource;
}

void markdown_core_resource_retain(markdown_core_resource *resource) {
    if (resource) {
        resource->holders++;
    }
}

void markdown_core_resource_release(markdown_core_mem *mem, markdown_core_resource *resource) {
    if (!resource) {
        return;
    }
    assert(resource->holders > 0);
    if (--resource->holders > 0) {
        return;
    }
    markdown_core_chunk_free(mem, &resource->url);
    markdown_core_optional_chunk_free(mem, &resource->title);
    mem->free(resource);
}

int markdown_core_node_set_extension(markdown_core_node *node, const markdown_core_extension *extension) {
    if (node == NULL) {
        return 0;
    }
    node->extension = extension;
    return 1;
}

int markdown_core_node_get_start_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_line;
}

int markdown_core_node_get_start_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_column;
}

int markdown_core_node_get_end_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_line;
}

int markdown_core_node_get_end_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_column;
}

// Unlink a node without adjusting its next, prev, and parent pointers.
static void S_node_unlink(markdown_core_node *node) {
    if (node == NULL) {
        return;
    }

    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }

    // Adjust first_child and last_child of parent.
    markdown_core_node *parent = node->parent;
    if (parent) {
        if (parent->first_child == node) {
            parent->first_child = node->next;
        }
        if (parent->last_child == node) {
            parent->last_child = node->prev;
        }
    }
}

void markdown_core_node_unlink(markdown_core_node *node) {
    S_node_unlink(node);

    node->next = NULL;
    node->prev = NULL;
    node->parent = NULL;
}

int markdown_core_node_insert_before(markdown_core_node *node, markdown_core_node *sibling) {
    if (node == NULL || sibling == NULL) {
        return 0;
    }

    /* A node cannot be its own sibling. `S_can_contain(node->parent, sibling)`
     * cannot see this: with `sibling == node`, the ancestor walk starts at the
     * PARENT and never meets the child, so it answers yes. The splice below
     * then unlinks the node and re-links it to itself -- measured,
     * `insert_before(b, b)` returns 1 and leaves `b->next == b` and
     * `b->prev == b`, an unbounded sibling list that any traversal walks
     * forever. That is D34, and the safety flag never covered it. */
    if (node == sibling) {
        return 0;
    }

    if (!node->parent || !S_can_contain(node->parent, sibling)) {
        return 0;
    }

    S_node_unlink(sibling);

    markdown_core_node *old_prev = node->prev;

    // Insert 'sibling' between 'old_prev' and 'node'.
    if (old_prev) {
        old_prev->next = sibling;
    }
    sibling->prev = old_prev;
    sibling->next = node;
    node->prev = sibling;

    // Set new parent.
    markdown_core_node *parent = node->parent;
    sibling->parent = parent;

    // Adjust first_child of parent if inserted as first child.
    if (parent && !old_prev) {
        parent->first_child = sibling;
    }

    return 1;
}

int markdown_core_node_insert_after(markdown_core_node *node, markdown_core_node *sibling) {
    if (node == NULL || sibling == NULL) {
        return 0;
    }

    /* A node cannot be its own sibling. `S_can_contain(node->parent, sibling)`
     * cannot see this: with `sibling == node`, the ancestor walk starts at the
     * PARENT and never meets the child, so it answers yes. The splice below
     * then unlinks the node and re-links it to itself -- measured,
     * `insert_before(b, b)` returns 1 and leaves `b->next == b` and
     * `b->prev == b`, an unbounded sibling list that any traversal walks
     * forever. That is D34, and the safety flag never covered it. */
    if (node == sibling) {
        return 0;
    }

    if (!node->parent || !S_can_contain(node->parent, sibling)) {
        return 0;
    }

    S_node_unlink(sibling);

    markdown_core_node *old_next = node->next;

    // Insert 'sibling' between 'node' and 'old_next'.
    if (old_next) {
        old_next->prev = sibling;
    }
    sibling->next = old_next;
    sibling->prev = node;
    node->next = sibling;

    // Set new parent.
    markdown_core_node *parent = node->parent;
    sibling->parent = parent;

    // Adjust last_child of parent if inserted as last child.
    if (parent && !old_next) {
        parent->last_child = sibling;
    }

    return 1;
}

int markdown_core_node_replace(markdown_core_node *oldnode, markdown_core_node *newnode) {
    if (!markdown_core_node_insert_before(oldnode, newnode)) {
        return 0;
    }
    markdown_core_node_unlink(oldnode);
    return 1;
}

int markdown_core_node_prepend_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }

    S_node_unlink(child);

    markdown_core_node *old_first_child = node->first_child;

    child->next = old_first_child;
    child->prev = NULL;
    child->parent = node;
    node->first_child = child;

    if (old_first_child) {
        old_first_child->prev = child;
    } else {
        // Also set last_child if node previously had no children.
        node->last_child = child;
    }

    return 1;
}

int markdown_core_node_append_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }

    S_node_unlink(child);

    markdown_core_node *old_last_child = node->last_child;

    child->next = NULL;
    child->prev = old_last_child;
    child->parent = node;
    node->last_child = child;

    if (old_last_child) {
        old_last_child->next = child;
    } else {
        // Also set first_child if node previously had no children.
        node->first_child = child;
    }

    return 1;
}

static void S_print_error(FILE *out, markdown_core_node *node, const char *elem) {
    if (out == NULL) {
        return;
    }
    fprintf(out, "Invalid '%s' in node type %s at %d:%d\n", elem, markdown_core_node_get_type_string(node),
            node->start_line, node->start_column);
}

int markdown_core_node_check(markdown_core_node *node, FILE *out) {
    markdown_core_node *cur;
    int errors = 0;

    if (!node) {
        return 0;
    }

    cur = node;
    for (;;) {
        if (cur->first_child) {
            if (cur->first_child->prev != NULL) {
                S_print_error(out, cur->first_child, "prev");
                cur->first_child->prev = NULL;
                ++errors;
            }
            if (cur->first_child->parent != cur) {
                S_print_error(out, cur->first_child, "parent");
                cur->first_child->parent = cur;
                ++errors;
            }
            cur = cur->first_child;
            continue;
        }

    next_sibling:
        if (cur == node) {
            break;
        }
        if (cur->next) {
            if (cur->next->prev != cur) {
                S_print_error(out, cur->next, "prev");
                cur->next->prev = cur;
                ++errors;
            }
            if (cur->next->parent != cur->parent) {
                S_print_error(out, cur->next, "parent");
                cur->next->parent = cur->parent;
                ++errors;
            }
            cur = cur->next;
            continue;
        }

        if (cur->parent->last_child != cur) {
            S_print_error(out, cur->parent, "last_child");
            cur->parent->last_child = cur;
            ++errors;
        }
        cur = cur->parent;
        goto next_sibling;
    }

    return errors;
}
