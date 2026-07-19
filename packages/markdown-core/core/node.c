#include <stdlib.h>
#include <string.h>

#include "config.h"
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
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    default:
        break;
    }

    return false;
}

bool markdown_core_node_owns_inlines(markdown_core_node *node) {
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

markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type, markdown_core_extension *extension) {
    return markdown_core_node_new_with_mem_and_ext(type, markdown_core_mem_default(), extension);
}

markdown_core_node *markdown_core_node_new_with_mem(markdown_core_node_type type, markdown_core_mem *mem) {
    return markdown_core_node_new_with_mem_and_ext(type, mem, NULL);
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_new_with_ext(type, NULL);
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
    default:
        break;
    }
}

// Free a markdown_core_node list and any children.
static void S_free_nodes(markdown_core_node *e) {
    markdown_core_node *next;
    while (e != NULL) {
        markdown_core_strbuf_free(&e->content);

        if (e->user_data && e->user_data_free_func) {
            e->user_data_free_func(NODE_MEM(e), e->user_data);
        }

        if (e->as.opaque && e->extension && e->extension->free_opaque) {
            e->extension->free_opaque(e->extension, NODE_MEM(e), e);
        }

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

int markdown_core_node_set_heading_level(markdown_core_node *node, int level) {
    if (node == NULL || level < 1 || level > 6) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading.level = level;
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

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.list_type;
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

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.list_type = type;
        return 1;
    } else {
        return 0;
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

int markdown_core_node_set_list_delim(markdown_core_node *node, markdown_core_delim_type delim) {
    if (!(delim == MARKDOWN_CORE_PERIOD_DELIM || delim == MARKDOWN_CORE_PAREN_DELIM)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.delimiter = delim;
        return 1;
    } else {
        return 0;
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

int markdown_core_node_set_list_start(markdown_core_node *node, int start) {
    if (node == NULL || start < 0) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.start = start;
        return 1;
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

int markdown_core_node_set_list_tight(markdown_core_node *node, int tight) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.tight = tight == 1;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_item_index(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return node->as.list.start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_item_index(markdown_core_node *node, int idx) {
    if (node == NULL || idx < 0) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST_ITEM) {
        node->as.list.start = idx;
        return 1;
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

int markdown_core_node_set_fence_info(markdown_core_node *node, const char *info) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code.info, info);
    } else {
        return 0;
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

int markdown_core_node_get_fenced(markdown_core_node *node, int *length, int *offset, char *character) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        *length = node->as.code.fence_length;
        *offset = node->as.code.fence_offset;
        *character = node->as.code.fence_char;
        return node->as.code.fenced;
    } else {
        return 0;
    }
}

int markdown_core_node_set_fenced(markdown_core_node *node, int fenced, int length, int offset, char character) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        node->as.code.fenced = (int8_t)fenced;
        node->as.code.fence_length = (uint8_t)length;
        node->as.code.fence_offset = (uint8_t)offset;
        node->as.code.fence_char = character;
        return 1;
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

int markdown_core_node_set_url(markdown_core_node *node, const char *url) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.link.url, url);
    default:
        break;
    }

    return 0;
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

int markdown_core_node_set_title(markdown_core_node *node, const char *title) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.link.title, title);
    default:
        break;
    }

    return 0;
}

markdown_core_extension *markdown_core_node_get_extension(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    return node->extension;
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
