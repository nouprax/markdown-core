#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "concrete_records.h"
#include "node.h"
#include "extension.h"

static void S_node_unlink(markdown_core_node *node);

#define NODE_MEM(node) markdown_core_node_mem(node)

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    if (child_type == MARKDOWN_CORE_NODE_DOCUMENT) {
        return false;
    }

    if (node->extension && node->extension->can_contain) {
        return node->extension->can_contain(node->extension, node, child_type) != 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_LIST:
        return child_type == MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_EMPHASIS:
    case MARKDOWN_CORE_NODE_STRONG:
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    default:
        break;
    }

    return false;
}

bool markdown_core_node_owns_inlines(markdown_core_node *node) {
    if (node->flags & MARKDOWN_CORE_NODE__OWNS_INLINE_SOURCE) {
        return true;
    }
    if (node->extension && node->extension->contains_inlines) {
        return node->extension->contains_inlines(node->extension, node) != 0;
    }
    return node->type == MARKDOWN_CORE_NODE_PARAGRAPH || node->type == MARKDOWN_CORE_NODE_HEADING;
}

static bool S_can_contain(markdown_core_node *node, markdown_core_node *child) {
    if (node == NULL || child == NULL) {
        return false;
    }
    if (NODE_MEM(node) != NODE_MEM(child)) {
        return 0;
    }

    // Verify that child is not an ancestor of node or equal to node. This is
    // O(depth) but only runs on the explicit node-mutation paths, never on
    // the per-line parsing hot path.
    for (markdown_core_node *cur = node; cur != NULL; cur = cur->parent) {
        if (cur == child) {
            return false;
        }
    }

    return markdown_core_node_can_contain_type(node, (markdown_core_node_type)child->type);
}

#ifndef NDEBUG
static void S_assert_same_node_domain(markdown_core_node *parent, markdown_core_node *child) {
    markdown_core_node *ancestor;

    assert(parent != NULL);
    assert(child != NULL);
    assert(parent != child);
    assert(NODE_MEM(parent) == NODE_MEM(child));
    assert(markdown_core_node_can_contain_type(parent, (markdown_core_node_type)child->type));

    for (ancestor = parent; ancestor; ancestor = ancestor->parent) {
        assert(ancestor != child);
    }
}
#endif

markdown_core_node *markdown_core_node_new_with_mem_and_ext(
    markdown_core_node_type type,
    markdown_core_mem *mem,
    markdown_core_extension *extension
) {
    markdown_core_node *node = (markdown_core_node *)mem->calloc(mem, 1, sizeof(*node));
    if (!node) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &node->content, 0);
    node->type = (uint16_t)type;
    node->extension = extension;

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading.level = 1;
        break;

    case MARKDOWN_CORE_NODE_LIST: {
        markdown_core_list *list = &node->as.list;
        list->list_type = MARKDOWN_CORE_BULLET_LIST;
        list->start = 0;
        list->tight = false;
        break;
    }

    default:
        break;
    }

    if (node->extension && node->extension->alloc_opaque) {
        node->extension->alloc_opaque(node->extension, mem, node);
    }

    return node;
}

markdown_core_node *markdown_core_node_new_with_mem(markdown_core_node_type type, markdown_core_mem *mem) {
    return markdown_core_node_new_with_mem_and_ext(type, mem, NULL);
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_new_with_mem(type, markdown_core_mem_default());
}

static void free_node_as(markdown_core_node *node) {
    switch (node->type) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.code.info);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.code.literal);
        break;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.literal);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.link.url);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.link.title);
        break;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.definition.label);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.definition.url);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.definition.title);
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.reference.label);
        break;
    default:
        break;
    }
}

/* One chunk made its own. A chunk with no data is NOT WRITTEN — a link
 * without a title, a definition without one — and stays that way: giving it
 * an owned empty string would turn "not written" into "written and empty",
 * which is a different projection (view_optional_equal in the facade tells
 * them apart), and a retired node whose projection changed by being retired
 * would move a revision that nothing about the document moved. */
static bool own_chunk(markdown_core_mem *mem, markdown_core_chunk *chunk) {
    return chunk->data == NULL || markdown_core_chunk_to_cstr(mem, chunk) != NULL;
}

bool markdown_core_node_own_chunks(markdown_core_node *node) {
    markdown_core_mem *mem = NODE_MEM(node);
    switch (node->type) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return own_chunk(mem, &node->as.code.info) && own_chunk(mem, &node->as.code.literal);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return own_chunk(mem, &node->as.literal);
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return own_chunk(mem, &node->as.link.url) && own_chunk(mem, &node->as.link.title);
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        return own_chunk(mem, &node->as.definition.label) && own_chunk(mem, &node->as.definition.url) &&
               own_chunk(mem, &node->as.definition.title);
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return own_chunk(mem, &node->as.reference.label);
    default:
        return true;
    }
}

// Free a markdown_core_node list and any children.
static void S_free_nodes(markdown_core_node *e) {
    markdown_core_node *next;
    while (e != NULL) {
        markdown_core_strbuf_free(&e->content);

        if (e->as.opaque && e->extension && e->extension->free_opaque) {
            e->extension->free_opaque(e->extension, NODE_MEM(e), e);
        }

        markdown_core_concrete_records_free(NODE_MEM(e), e->concrete);
        markdown_core_inline_concrete_records_free(NODE_MEM(e), e->inline_concrete);
        markdown_core_probes_free(e->probes);

        free_node_as(e);

        if (e->last_child) {
            // Splice children into list
            e->last_child->next = e->next;
            e->next = e->first_child;
        }
        next = e->next;
        NODE_MEM(e)->free(NODE_MEM(e), e);
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
        return (markdown_core_node_type)node->type;
    }
}

int markdown_core_node_set_type(markdown_core_node *node, markdown_core_node_type type) {
    markdown_core_node_type initial_type;

    if (type == node->type) {
        return 1;
    }

    initial_type = (markdown_core_node_type)node->type;
    node->type = (uint16_t)type;

    if (!S_can_contain(node->parent, node)) {
        node->type = (uint16_t)initial_type;
        return 0;
    }

    /* We rollback the type to free the union members appropriately */
    node->type = (uint16_t)initial_type;
    free_node_as(node);

    node->type = (uint16_t)type;

    return 1;
}

void markdown_core_node_set_type_unchecked(markdown_core_node *node, markdown_core_node_type type) {
    assert(node != NULL);
    assert(node->parent == NULL || NODE_MEM(node->parent) == NODE_MEM(node));
    assert(
        ((markdown_core_node_type)node->type & MARKDOWN_CORE_NODE_TYPE_MASK) == (type & MARKDOWN_CORE_NODE_TYPE_MASK)
    );
    assert(node->parent == NULL || markdown_core_node_can_contain_type(node->parent, type));

    if (type == node->type) {
        return;
    }

    free_node_as(node);
    node->type = (uint16_t)type;
}

const char *markdown_core_node_get_type_string(markdown_core_node *node) {
    if (node == NULL) {
        return "NONE";
    }

    if (node->extension && node->extension->get_type_string) {
        return node->extension->get_type_string(node->extension, node);
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_NONE:
        return "none";
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return "document";
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
        return "block_quote";
    case MARKDOWN_CORE_NODE_LIST:
        return "list";
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return "list_item";
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return "code_block";
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return "html_block";
    case MARKDOWN_CORE_NODE_PARAGRAPH:
        return "paragraph";
    case MARKDOWN_CORE_NODE_HEADING:
        return "heading";
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return "thematic_break";
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
    case MARKDOWN_CORE_NODE_EMPHASIS:
        return "emphasis";
    case MARKDOWN_CORE_NODE_STRONG:
        return "strong";
    case MARKDOWN_CORE_NODE_LINK:
        return "link";
    case MARKDOWN_CORE_NODE_IMAGE:
        return "image";
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return "footnote_definition";
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        return "footnote_reference";
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        return "reference_definition";
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
        return "link_reference";
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return "image_reference";
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

const char *markdown_core_node_get_literal(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.literal);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code.literal);

    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_literal(markdown_core_node *node, const char *content) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.literal, content);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code.literal, content);

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

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        return node->as.heading.level;

    default:
        break;
    }

    return 0;
}

markdown_core_list_type markdown_core_node_get_list_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_LIST;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.list_type;
    } else {
        return MARKDOWN_CORE_NO_LIST;
    }
}

markdown_core_delim_type markdown_core_node_get_list_delim(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_DELIM;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.delimiter;
    } else {
        return MARKDOWN_CORE_NO_DELIM;
    }
}

int markdown_core_node_get_list_start(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.start;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_tight(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.tight;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_fence_info(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code.info);
    } else {
        return NULL;
    }
}

int markdown_core_node_get_fence_closed(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return node->as.code.fenced && node->as.code.fence_closed;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_url(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.link.url);
    default:
        break;
    }

    return NULL;
}

const char *markdown_core_node_get_title(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.link.title);
    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_extension(markdown_core_node *node, markdown_core_extension *extension) {
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

void markdown_core_node_insert_before_unchecked(markdown_core_node *node, markdown_core_node *sibling) {
    markdown_core_node *old_prev;
    markdown_core_node *parent;

    assert(node != NULL);
    assert(sibling != NULL);
    assert(node != sibling);
    assert(node->parent != NULL);
    assert(NODE_MEM(node->parent) == NODE_MEM(node));
    assert(markdown_core_node_can_contain_type(node->parent, (markdown_core_node_type)node->type));
#ifndef NDEBUG
    S_assert_same_node_domain(node->parent, sibling);
#endif

    S_node_unlink(sibling);
    old_prev = node->prev;
    parent = node->parent;
    if (old_prev) {
        old_prev->next = sibling;
    }
    sibling->prev = old_prev;
    sibling->next = node;
    sibling->parent = parent;
    node->prev = sibling;
    if (parent && !old_prev) {
        parent->first_child = sibling;
    }
}

void markdown_core_node_insert_after_unchecked(markdown_core_node *node, markdown_core_node *sibling) {
    markdown_core_node *old_next;
    markdown_core_node *parent;

    assert(node != NULL);
    assert(sibling != NULL);
    assert(node != sibling);
    assert(node->parent != NULL);
    assert(NODE_MEM(node->parent) == NODE_MEM(node));
    assert(markdown_core_node_can_contain_type(node->parent, (markdown_core_node_type)node->type));
#ifndef NDEBUG
    S_assert_same_node_domain(node->parent, sibling);
#endif

    S_node_unlink(sibling);
    old_next = node->next;
    parent = node->parent;
    sibling->parent = parent;
    sibling->prev = node;
    sibling->next = old_next;
    node->next = sibling;
    if (old_next) {
        old_next->prev = sibling;
    } else {
        parent->last_child = sibling;
    }
}

int markdown_core_node_insert_after(markdown_core_node *node, markdown_core_node *sibling) {
    if (node == NULL || sibling == NULL) {
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

void markdown_core_node_append_child_unchecked(markdown_core_node *node, markdown_core_node *child) {
    markdown_core_node *old_last_child;

#ifndef NDEBUG
    S_assert_same_node_domain(node, child);
#endif

    S_node_unlink(child);
    old_last_child = node->last_child;
    child->next = NULL;
    child->prev = old_last_child;
    child->parent = node;
    node->last_child = child;
    if (old_last_child) {
        old_last_child->next = child;
    } else {
        node->first_child = child;
    }
}

static void S_print_error(FILE *out, markdown_core_node *node, const char *elem) {
    if (out == NULL) {
        return;
    }
    fprintf(
        out,
        "Invalid '%s' in node type %s at %d:%d\n",
        elem,
        markdown_core_node_get_type_string(node),
        node->start_line,
        node->start_column
    );
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

// FNV-1a. A literal enters as its LENGTH plus a bounded sample of its bytes,
// not all of them: hashing every byte would make this O(document bytes), and
// confusing two literals that share a length and both ends is affordable for
// the reason stated on the field (node.h).
#define MARKDOWN_CORE_HASH_SAMPLE 32u

uint64_t markdown_core_hash_mix(uint64_t h, uint64_t value) {
    h = (h ^ value) * 0x100000001b3ull;
    return h ^ (h >> 29);
}

uint64_t markdown_core_hash_bytes(uint64_t h, const uint8_t *data, size_t length) {
    size_t head;
    size_t tail;
    size_t i;

    if (!data) {
        return markdown_core_hash_mix(h, 0);
    }
    head = length < MARKDOWN_CORE_HASH_SAMPLE ? length : MARKDOWN_CORE_HASH_SAMPLE;
    tail = length - head < MARKDOWN_CORE_HASH_SAMPLE ? length - head : MARKDOWN_CORE_HASH_SAMPLE;
    h = (h ^ (uint64_t)length) * 0x100000001b3ull;
    for (i = 0; i < head; i++) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    for (i = 0; i < tail; i++) {
        h ^= data[length - tail + i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t hash_chunk(uint64_t h, const markdown_core_chunk *chunk) {
    if (!chunk->data || chunk->len < 0) {
        return markdown_core_hash_mix(h, 0);
    }
    return markdown_core_hash_bytes(h, chunk->data, (size_t)chunk->len);
}

/* WHAT GOES IN, and why this list and not another: the scalars below are
 * exactly the ones markdown_core_ast_projection_changed (extensions/ast.c) reads
 * to decide that two nodes of the same kind differ in VALUE. A field it
 * compares and this omits is a field on which two different nodes hash
 * equal, and the prefix sweep in diff.c then PAIRS them -- which is how an
 * inserted `## Same` in front of an existing `# Same` took the H1's id and
 * left the surviving H1 to be minted fresh. The delta stayed honest, because
 * a paired node is still compared field by field; what was wrong was which
 * node the consumer's row state stayed attached to.
 *
 * Values core cannot reach are mixed by the extension that owns them,
 * through `hash_value` -- a directive's name and attributes, a table's
 * alignments, a row's header bit, a formula's mode. An extension that
 * registers a node type and does not implement it leaves its own nodes
 * pairing on type and children alone. */
uint64_t markdown_core_node_stamp_own(const markdown_core_node *node) {
    uint64_t h = 0xcbf29ce484222325ull;

    h = markdown_core_hash_mix(h, (uint64_t)node->type);
    switch (node->type) {
    // The raw type, not the facade kind: this runs on every node of every
    // parse, and for these the union member IS the literal chunk.
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    // A footnote's label is its own literal, and it is what tells two
    // otherwise identical definitions apart.
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        h = hash_chunk(h, &node->as.literal);
        break;
    // `as.literal` aliases `as.code.info`, so reading it here hashed a code
    // block's INFO STRING while meaning its body: two blocks sharing a fence
    // language hashed equal however far apart their contents were.
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        h = hash_chunk(h, &node->as.code.literal);
        h = hash_chunk(h, &node->as.code.info);
        h = markdown_core_hash_mix(h, (uint64_t)(node->as.code.fenced != 0));
        h = markdown_core_hash_mix(h, (uint64_t)(node->as.code.fence_closed != 0));
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        h = markdown_core_hash_mix(h, (uint64_t)node->as.heading.level);
        break;
    case MARKDOWN_CORE_NODE_LIST:
        h = markdown_core_hash_mix(h, (uint64_t)node->as.list.list_type);
        h = markdown_core_hash_mix(h, (uint64_t)(node->as.list.tight != 0));
        // The start ordinal is the list's only when it is ordered; a bullet
        // list has none, which is what the facade reports.
        if (node->as.list.list_type == MARKDOWN_CORE_ORDERED_LIST) {
            h = markdown_core_hash_mix(h, (uint64_t)node->as.list.start);
        }
        break;
    // The checkbox is an extension's, present only where one claimed the
    // item; `extension` is the same gate the facade uses, minus its strcmp.
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        h = markdown_core_hash_mix(h, node->extension ? (uint64_t)(node->as.list.checked != 0) + 1u : 0u);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        h = hash_chunk(h, &node->as.link.url);
        h = hash_chunk(h, &node->as.link.title);
        break;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        h = hash_chunk(h, &node->as.definition.label);
        h = hash_chunk(h, &node->as.definition.url);
        h = hash_chunk(h, &node->as.definition.title);
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        h = hash_chunk(h, &node->as.reference.label);
        h = markdown_core_hash_mix(h, (uint64_t)node->as.reference.form);
        break;
    default:
        break;
    }
    // Whatever the core union cannot reach: a directive's name and
    // attributes, a table's alignments, a row's header bit, a formula's
    // mode. The extension that registered the type is the only thing that
    // can read them, so it is the thing that mixes them.
    if (node->extension && node->extension->hash_value) {
        h = node->extension->hash_value(node->extension, (markdown_core_node *)node, h);
    }
    return h;
}

uint64_t markdown_core_node_hash_weight(uint32_t index) {
    uint64_t result = 1;
    uint64_t base = MARKDOWN_CORE_NODE_HASH_STEP;
    uint64_t exponent = (uint64_t)index + 1;
    while (exponent) {
        if (exponent & 1) {
            result *= base;
        }
        base *= base;
        exponent >>= 1;
    }
    return result;
}

uint64_t markdown_core_node_hash_children(const markdown_core_node *node, uint64_t h, markdown_core_node *from) {
    markdown_core_node *child;
    uint32_t index;
    uint64_t weight;
    if (!from) {
        return h;
    }
    index = from->prev ? from->prev->hash_index + 1 : 0;
    weight = markdown_core_node_hash_weight(index);
    for (child = from; child; child = child->next) {
        child->hash_index = index;
        h += child->subtree_hash * weight;
        if (child == node->last_child) {
            break;
        }
        index++;
        weight *= MARKDOWN_CORE_NODE_HASH_STEP;
    }
    return h;
}

void markdown_core_node_stamp_from(markdown_core_node *node, uint64_t prefix, markdown_core_node *from) {
    node->subtree_hash = markdown_core_node_hash_children(node, prefix, from);
}

void markdown_core_node_stamp(markdown_core_node *node) {
    markdown_core_node_stamp_from(node, markdown_core_node_stamp_own(node), node->first_child);
}

void markdown_core_node_stamp_tree(markdown_core_node *root) {
    markdown_core_node *node = root;

    /* Postorder, so a node is stamped only once its children carry their own
     * hashes. Iterative and allocation-free: document depth costs no stack.
     *
     * It sits beside markdown_core_node_stamp because that is where a node
     * operation belongs, NOT for speed -- putting the walk in the same
     * translation unit so the per-node stamp could inline was measured and
     * changed nothing (2.745 s against 2.743 s on multiple_duplicate_references).
     * The pass costs what it costs because it is one more traversal of every
     * node in the tree; neither the call, the iterator, nor the literal
     * sampling is the term that matters. */
    for (;;) {
        while (node->first_child) {
            node = node->first_child;
        }
        for (;;) {
            markdown_core_node_stamp(node);
            if (node == root) {
                return;
            }
            if (node->next) {
                node = node->next;
                break;
            }
            node = node->parent;
        }
    }
}
