/**
 * Block parsing implementation.
 *
 * For a high-level overview of the block parsing process,
 * see http://spec.commonmark.org/0.24/#phase-1-block-structure
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "markdown_core_ctype.h"
#include "syntax_extension.h"
#include "config.h"
#include "parser.h"
#include "markdown-core.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "footnotes.h"
#include "iterator.h"

#define CODE_INDENT 4
#define TAB_STOP 4

/**
 * Very deeply nested lists can cause quadratic performance issues.
 * This constant is used in open_new_blocks() to limit the nesting
 * depth. It is unlikely that a non-contrived markdown document will
 * be nested this deeply.
 */
#define MAX_LIST_DEPTH 100

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

static bool S_html_literal_starts_with_comment(markdown_core_node *node) {
    markdown_core_chunk *literal;
    bufsize_t offset = 0;

    if (node->type != MARKDOWN_CORE_NODE_HTML_BLOCK && node->type != MARKDOWN_CORE_NODE_HTML) {
        return false;
    }

    literal = &node->as.literal;

    if (node->type == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        while (offset < literal->len && (literal->data[offset] == ' ' || literal->data[offset] == '\t')) {
            offset++;
        }
    }

    return literal->len - offset >= 4 && memcmp(literal->data + offset, "<!--", 4) == 0;
}

static bool S_strip_html_comments(markdown_core_node *root) {
    bool stripped = false;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;

    if (!iter)
        return false;

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && S_html_literal_starts_with_comment(node)) {
            markdown_core_node_free(node);
            stripped = true;
        }
    }

    markdown_core_iter_free(iter);

    if (stripped) {
        return markdown_core_consolidate_text_nodes(root) != 0;
    }
    return true;
}

static bool S_last_line_blank(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

static bool S_last_line_checked(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED) != 0;
}

static MARKDOWN_CORE_INLINE markdown_core_node_type S_type(const markdown_core_node *node) {
    return (markdown_core_node_type)node->type;
}

static void S_set_last_line_blank(markdown_core_node *node, bool is_blank) {
    if (is_blank)
        node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    else
        node->flags &= ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
}

static void S_set_last_line_checked(markdown_core_node *node) { node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_CHECKED; }

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static MARKDOWN_CORE_INLINE bool S_is_space_or_tab(char c) { return (c == ' ' || c == '\t'); }

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(markdown_core_mem *mem, markdown_core_node_type tag, int start_line,
                                      int start_column) {
    markdown_core_node *e;

    e = (markdown_core_node *)mem->calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    markdown_core_strbuf_init(mem, &e->content, 32);
    e->type = (uint16_t)tag;
    e->flags = MARKDOWN_CORE_NODE__OPEN;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;

    return e;
}

// Create a root document node.
static markdown_core_node *make_document(markdown_core_mem *mem) {
    markdown_core_node *e = make_block(mem, MARKDOWN_CORE_NODE_DOCUMENT, 1, 1);
    return e;
}

/* Appends and reports failure directly instead of relying on llist_append's
 * silent-drop behavior. */
static int S_llist_append_checked(markdown_core_mem *mem, markdown_core_llist **head, void *data) {
    markdown_core_llist *node = (markdown_core_llist *)mem->calloc(1, sizeof(*node));
    markdown_core_llist *tail;
    if (!node)
        return 0;
    node->data = data;
    node->next = NULL;
    if (!*head) {
        *head = node;
        return 1;
    }
    for (tail = *head; tail->next; tail = tail->next)
        ;
    tail->next = node;
    return 1;
}

int markdown_core_parser_attach_syntax_extension(markdown_core_parser *parser,
                                                 markdown_core_syntax_extension *extension) {
    if (!S_llist_append_checked(parser->mem, &parser->syntax_extensions, extension))
        return 0;
    if (extension->match_inline || extension->insert_inline_from_delim) {
        if (!S_llist_append_checked(parser->mem, &parser->inline_syntax_extensions, extension))
            return 0;
    }

    return 1;
}

static void markdown_core_parser_dispose(markdown_core_parser *parser) {
    if (parser->root)
        markdown_core_node_free(parser->root);

    if (parser->refmap)
        markdown_core_map_free(parser->refmap);
}

static void markdown_core_parser_reset(markdown_core_parser *parser) {
    markdown_core_llist *saved_exts = parser->syntax_extensions;
    markdown_core_llist *saved_inline_exts = parser->inline_syntax_extensions;
    int saved_options = parser->options;
    markdown_core_mem *saved_mem = parser->mem;

    markdown_core_parser_dispose(parser);

    memset(parser, 0, sizeof(markdown_core_parser));
    parser->mem = saved_mem;

    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->linebuf, 0);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->root = document;
    parser->current = document;

    parser->syntax_extensions = saved_exts;
    parser->inline_syntax_extensions = saved_inline_exts;
    parser->options = saved_options;

    /* A reset that could not rebuild its structures poisons the parser: feed
     * becomes a no-op and finish reports failure. */
    if (!parser->root || !parser->refmap || parser->curline.oom)
        parser->oom = true;

    markdown_core_inlines_reset_special_chars(parser);
}

markdown_core_parser *markdown_core_parser_new_with_mem(int options, markdown_core_mem *mem) {
    markdown_core_parser *parser = (markdown_core_parser *)mem->calloc(1, sizeof(markdown_core_parser));
    if (!parser)
        return NULL;
    parser->mem = mem;
    parser->options = options;
    markdown_core_parser_reset(parser);
    return parser;
}

markdown_core_parser *markdown_core_parser_new(int options) {
    extern markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;
    return markdown_core_parser_new_with_mem(options, &MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR);
}

void markdown_core_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem = parser->mem;
    markdown_core_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);
    markdown_core_llist_free(parser->mem, parser->syntax_extensions);
    markdown_core_llist_free(parser->mem, parser->inline_syntax_extensions);
    mem->free(parser);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b);

// Returns true if line has only space characters, else false.
static bool is_blank(markdown_core_strbuf *s, bufsize_t offset) {
    while (offset < s->size) {
        switch (s->ptr[offset]) {
        case '\r':
        case '\n':
            return true;
        case ' ':
            offset++;
            break;
        case '\t':
            offset++;
            break;
        default:
            return false;
        }
    }

    return true;
}

static MARKDOWN_CORE_INLINE bool extension_accepts_lines(markdown_core_node *node) {
    return node->extension && node->extension->accepts_lines_func &&
           node->extension->accepts_lines_func(node->extension, node) != 0;
}

static MARKDOWN_CORE_INLINE bool accepts_lines(markdown_core_node *node) {
    markdown_core_node_type block_type = S_type(node);

    if (extension_accepts_lines(node))
        return true;

    return (block_type == MARKDOWN_CORE_NODE_PARAGRAPH || block_type == MARKDOWN_CORE_NODE_HEADING ||
            block_type == MARKDOWN_CORE_NODE_CODE_BLOCK);
}

static MARKDOWN_CORE_INLINE bool contains_inlines(markdown_core_node *node) {
    if (node->extension && node->extension->contains_inlines_func) {
        return node->extension->contains_inlines_func(node->extension, node) != 0;
    }

    return (node->type == MARKDOWN_CORE_NODE_PARAGRAPH || node->type == MARKDOWN_CORE_NODE_HEADING);
}

static void add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    if (parser->partially_consumed_tab) {
        parser->offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    markdown_core_strbuf_put(&node->content, ch->data + parser->offset, ch->len - parser->offset);
    if (node->content.oom)
        parser->oom = true;
}

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    bufsize_t i;
    unsigned char c;

    for (i = ln->size - 1; i >= 0; --i) {
        c = ln->ptr[i];

        if (c != ' ' && c != '\t' && !S_is_line_end_char(c))
            break;
    }

    if (i < 0) {
        markdown_core_strbuf_clear(ln);
        return;
    }

    for (; i < ln->size; ++i) {
        c = ln->ptr[i];

        if (!S_is_line_end_char(c))
            continue;

        markdown_core_strbuf_truncate(ln, i);
        break;
    }
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool S_ends_with_blank_line(markdown_core_node *node) {
    if (S_last_line_checked(node)) {
        return (S_last_line_blank(node));
    } else if ((S_type(node) == MARKDOWN_CORE_NODE_LIST || S_type(node) == MARKDOWN_CORE_NODE_LIST_ITEM) &&
               node->last_child) {
        S_set_last_line_checked(node);
        return (S_ends_with_blank_line(node->last_child));
    } else {
        S_set_last_line_checked(node);
        return (S_last_line_blank(node));
    }
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    while (chunk.len && chunk.data[0] == '[' &&
           (pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap))) {

        chunk.data += pos;
        chunk.len -= pos;
    }
    markdown_core_strbuf_drop(node_content, (node_content->size - chunk.len));
    return !is_blank(&b->content, 0);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_node *item;
    markdown_core_node *subitem;
    markdown_core_node *parent;
    bool has_content;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call finalize on closed blocks
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;

    if (parser->curline.size == 0) {
        // end of input - line number has not been incremented
        b->end_line = parser->line_number;
        b->end_column = parser->last_line_length;
    } else if (S_type(b) == MARKDOWN_CORE_NODE_DOCUMENT ||
               (S_type(b) == MARKDOWN_CORE_NODE_CODE_BLOCK && b->as.code.fenced) ||
               (S_type(b) == MARKDOWN_CORE_NODE_HEADING && b->as.heading.setext)) {
        b->end_line = parser->line_number;
        b->end_column = parser->curline.size;
        if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n')
            b->end_column -= 1;
        if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r')
            b->end_column -= 1;
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    markdown_core_strbuf *node_content = &b->content;

    switch (S_type(b)) {
    case MARKDOWN_CORE_NODE_PARAGRAPH: {
        has_content = resolve_reference_link_definitions(parser, b);
        if (!has_content) {
            // remove blank node (former reference def)
            markdown_core_node_free(b);
        }
        break;
    }

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        if (!b->as.code.fenced) { // indented code
            remove_trailing_blank_lines(node_content);
            markdown_core_strbuf_putc(node_content, '\n');
        } else {
            // first line of contents becomes info
            for (pos = 0; pos < node_content->size; ++pos) {
                if (S_is_line_end_char(node_content->ptr[pos]))
                    break;
            }
            assert(pos < node_content->size);

            markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT(parser->mem);
            houdini_unescape_html_f(&tmp, node_content->ptr, pos);
            markdown_core_strbuf_trim(&tmp);
            markdown_core_strbuf_unescape(&tmp);
            b->as.code.info = markdown_core_chunk_buf_detach(&tmp);
            if (!b->as.code.info.data)
                parser->oom = true;

            if (node_content->ptr[pos] == '\r')
                pos += 1;
            if (node_content->ptr[pos] == '\n')
                pos += 1;
            markdown_core_strbuf_drop(node_content, pos);
        }
        b->as.code.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.code.literal.data)
            parser->oom = true;
        break;

    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        b->as.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.literal.data)
            parser->oom = true;
        break;

    case MARKDOWN_CORE_NODE_LIST: // determine tight/loose status
        b->as.list.tight = true;  // tight by default
        item = b->first_child;

        while (item) {
            // check for non-final non-empty list item ending with blank line:
            if (S_last_line_blank(item) && item->next) {
                b->as.list.tight = false;
                break;
            }
            // recurse into children of list item, to see if there are
            // spaces between them:
            subitem = item->first_child;
            while (subitem) {
                if ((item->next || subitem->next) && S_ends_with_blank_line(subitem)) {
                    b->as.list.tight = false;
                    break;
                }
                subitem = subitem->next;
            }
            if (!(b->as.list.tight)) {
                break;
            }
            item = item->next;
        }

        break;

    default:
        break;
    }

    return parent;
}

// Add a node as child of another.  Return pointer to child.
static markdown_core_node *add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_node_type block_type, int start_column) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        parent = finalize(parser, parent);
    }

    markdown_core_node *child = make_block(parser->mem, block_type, parser->line_number, start_column);
    if (!child || child->content.oom) {
        parser->oom = true;
        if (child)
            markdown_core_node_free(child);
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->current = parent;
        return NULL;
    }
    child->parent = parent;

    if (parent->last_child) {
        parent->last_child->next = child;
        child->prev = parent->last_child;
    } else {
        parent->first_child = child;
        child->prev = NULL;
    }
    parent->last_child = child;
    return child;
}

void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_syntax_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        markdown_core_syntax_extension *ext = (markdown_core_syntax_extension *)tmp_ext->data;
        markdown_core_llist *tmp_char;
        for (tmp_char = ext->special_inline_chars; tmp_char; tmp_char = tmp_char->next) {
            unsigned char c = (unsigned char)(size_t)tmp_char->data;
            if (add)
                markdown_core_inlines_add_special_character(parser, c, ext->emphasis);
            else
                markdown_core_inlines_remove_special_character(parser, c, ext->emphasis);
        }
    }
}

// Walk through node and all children, recursively, parsing
// string content into inline content where appropriate.
static void process_inlines(markdown_core_parser *parser, markdown_core_map *refmap, int options) {
    markdown_core_iter *iter = markdown_core_iter_new(parser->root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    if (!iter) {
        parser->oom = true;
        return;
    }

    markdown_core_manage_extensions_special_characters(parser, true);

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            if (contains_inlines(cur)) {
                markdown_core_parse_inlines(parser, cur, refmap, options);
            }
        }
    }

    markdown_core_manage_extensions_special_characters(parser, false);

    markdown_core_iter_free(iter);
}

static int sort_footnote_by_ix(const void *_a, const void *_b) {
    markdown_core_footnote *a = *(markdown_core_footnote **)_a;
    markdown_core_footnote *b = *(markdown_core_footnote **)_b;
    return (int)a->ix - (int)b->ix;
}

static void process_footnotes(markdown_core_parser *parser) {
    // * Collect definitions in a map.
    // * Iterate the references in the document in order, assigning indices to
    //   definitions in the order they're seen.
    // * Write out the footnotes at the bottom of the document in index order.

    markdown_core_map *map = markdown_core_footnote_map_new(parser->mem);
    if (!map) {
        parser->oom = true;
        return;
    }

    markdown_core_iter *iter = markdown_core_iter_new(parser->root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    if (!iter) {
        parser->oom = true;
        markdown_core_map_free(map);
        return;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && cur->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            markdown_core_footnote_create(map, cur);
        }
    }

    markdown_core_iter_free(iter);
    iter = markdown_core_iter_new(parser->root);
    unsigned int ix = 0;

    if (!iter) {
        parser->oom = true;
        markdown_core_unlink_footnotes_map(map);
        markdown_core_map_free(map);
        return;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && cur->type == MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE) {
            markdown_core_footnote *footnote =
                (markdown_core_footnote *)markdown_core_map_lookup(map, &cur->as.literal);
            if (footnote) {
                if (!footnote->ix)
                    footnote->ix = ++ix;

                // store a reference to this footnote reference's footnote definition
                // this is used by renderers when generating label ids
                cur->parent_footnote_def = footnote->node;

                // keep track of a) count of how many times this footnote def has been
                // referenced, and b) which reference index this footnote ref is at.
                // this is used by renderers when generating links and backreferences.
                cur->footnote.ref_ix = ++footnote->node->footnote.def_count;

                char n[32];
                snprintf(n, sizeof(n), "%d", footnote->ix);
                markdown_core_chunk_free(parser->mem, &cur->as.literal);
                markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(parser->mem);
                markdown_core_strbuf_puts(&buf, n);

                cur->as.literal = markdown_core_chunk_buf_detach(&buf);
            } else {
                markdown_core_node *text = (markdown_core_node *)parser->mem->calloc(1, sizeof(*text));
                /* On allocation failure keep the unresolved reference node
                 * and report the loss. */
                if (text) {
                    markdown_core_strbuf_init(parser->mem, &text->content, 0);
                    text->type = (uint16_t)MARKDOWN_CORE_NODE_TEXT;

                    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(parser->mem);
                    markdown_core_strbuf_puts(&buf, "[^");
                    markdown_core_strbuf_put(&buf, cur->as.literal.data, cur->as.literal.len);
                    markdown_core_strbuf_putc(&buf, ']');

                    text->as.literal = markdown_core_chunk_buf_detach(&buf);
                    if (!text->as.literal.data)
                        parser->oom = true;
                    markdown_core_node_insert_after(cur, text);
                    markdown_core_node_free(cur);
                } else {
                    parser->oom = true;
                }
            }
        }
    }

    markdown_core_iter_free(iter);

    if (map->prepared) {
        markdown_core_map_entry **footnotes = map->sorted;
        if (map->indexed) {
            footnotes = (markdown_core_map_entry **)parser->mem->calloc(map->size, sizeof(*footnotes));
            if (!footnotes)
                parser->oom = true;
            if (footnotes) {
                size_t slot;
                size_t count = 0;
                for (slot = 0; slot < map->index.capacity; slot++) {
                    if (map->index.slots[slot].key)
                        footnotes[count++] = (markdown_core_map_entry *)map->index.slots[slot].value;
                }
                assert(count == map->size);
            }
        }
        /* When the collection array cannot be allocated, skip emission; the
         * definitions are then unlinked and freed with the map below. */
        if (footnotes) {
            qsort(footnotes, map->size, sizeof(markdown_core_map_entry *), sort_footnote_by_ix);
            for (unsigned int i = 0; i < map->size; ++i) {
                markdown_core_footnote *footnote = (markdown_core_footnote *)footnotes[i];
                if (!footnote->ix) {
                    markdown_core_node_unlink(footnote->node);
                    continue;
                }
                markdown_core_node_append_child(parser->root, footnote->node);
                footnote->node = NULL;
            }
            if (map->indexed)
                parser->mem->free(footnotes);
        }
    }

    if (map->oom)
        parser->oom = true;

    markdown_core_unlink_footnotes_map(map);
    markdown_core_map_free(map);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static bufsize_t parse_list_marker(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t pos,
                                   bool interrupts_paragraph, markdown_core_list **dataptr) {
    markdown_core_mem *mem = parser->mem;
    unsigned char c;
    bufsize_t startpos;
    markdown_core_list *data;
    bufsize_t i;

    startpos = pos;
    c = peek_at(input, pos);

    if (c == '*' || c == '-' || c == '+') {
        pos++;
        if (!markdown_core_isspace(peek_at(input, pos))) {
            return 0;
        }

        if (interrupts_paragraph) {
            i = pos;
            // require non-blank content after list marker:
            while (S_is_space_or_tab(peek_at(input, i))) {
                i++;
            }
            if (peek_at(input, i) == '\n') {
                return 0;
            }
        }

        data = (markdown_core_list *)mem->calloc(1, sizeof(*data));
        if (!data) {
            /* Allocation loss, not an invalid marker. */
            parser->oom = true;
            return 0;
        }
        data->marker_offset = 0; // will be adjusted later
        data->list_type = MARKDOWN_CORE_BULLET_LIST;
        data->bullet_char = c;
        data->start = 0;
        data->delimiter = MARKDOWN_CORE_NO_DELIM;
        data->tight = false;
    } else if (markdown_core_isdigit(c)) {
        int start = 0;
        int digits = 0;

        do {
            start = (10 * start) + (peek_at(input, pos) - '0');
            pos++;
            digits++;
            // We limit to 9 digits to avoid overflow,
            // assuming max int is 2^31 - 1
            // This also seems to be the limit for 'start' in some browsers.
        } while (digits < 9 && markdown_core_isdigit(peek_at(input, pos)));

        if (interrupts_paragraph && start != 1) {
            return 0;
        }
        c = peek_at(input, pos);
        if (c == '.' || c == ')') {
            pos++;
            if (!markdown_core_isspace(peek_at(input, pos))) {
                return 0;
            }
            if (interrupts_paragraph) {
                // require non-blank content after list marker:
                i = pos;
                while (S_is_space_or_tab(peek_at(input, i))) {
                    i++;
                }
                if (S_is_line_end_char(peek_at(input, i))) {
                    return 0;
                }
            }

            data = (markdown_core_list *)mem->calloc(1, sizeof(*data));
            if (!data) {
                parser->oom = true;
                return 0;
            }
            data->marker_offset = 0; // will be adjusted later
            data->list_type = MARKDOWN_CORE_ORDERED_LIST;
            data->bullet_char = 0;
            data->start = start;
            data->delimiter = (c == '.' ? MARKDOWN_CORE_PERIOD_DELIM : MARKDOWN_CORE_PAREN_DELIM);
            data->tight = false;
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    *dataptr = data;
    return (pos - startpos);
}

// Return 1 if list item belongs in list, else 0.
static int lists_match(markdown_core_list *list_data, markdown_core_list *item_data) {
    return (list_data->list_type == item_data->list_type && list_data->delimiter == item_data->delimiter &&
            // list_data->marker_offset == item_data.marker_offset &&
            list_data->bullet_char == item_data->bullet_char);
}

static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = finalize(parser, parser->current);
    }

    finalize(parser, parser->root);

    // Limit total size of extra content created from reference links to
    // document size to avoid superlinear growth. Always allow 100KB.
    if (parser->refmap) {
        if (parser->total_size > 100000)
            parser->refmap->max_ref_size = parser->total_size;
        else
            parser->refmap->max_ref_size = 100000;
    }

    process_inlines(parser, parser->refmap, parser->options);
    if (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES)
        process_footnotes(parser);

    return parser->root;
}

markdown_core_node *markdown_core_parse_file(FILE *f, int options) {
    unsigned char buffer[4096];
    markdown_core_parser *parser = markdown_core_parser_new(options);
    size_t bytes;
    markdown_core_node *document;

    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        bool eof = bytes < sizeof(buffer);
        S_parser_feed(parser, buffer, bytes, eof);
        if (eof) {
            break;
        }
    }

    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return document;
}

markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len, int options) {
    markdown_core_parser *parser = markdown_core_parser_new(options);
    markdown_core_node *document;

    S_parser_feed(parser, (const unsigned char *)buffer, len, true);

    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return document;
}

void markdown_core_parser_feed(markdown_core_parser *parser, const char *buffer, size_t len) {
    S_parser_feed(parser, (const unsigned char *)buffer, len, false);
}

void markdown_core_parser_feed_reentrant(markdown_core_parser *parser, const char *buffer, size_t len) {
    markdown_core_strbuf saved_linebuf;

    markdown_core_strbuf_init(parser->mem, &saved_linebuf, 0);
    markdown_core_strbuf_puts(&saved_linebuf, markdown_core_strbuf_cstr(&parser->linebuf));
    markdown_core_strbuf_clear(&parser->linebuf);

    S_parser_feed(parser, (const unsigned char *)buffer, len, true);

    markdown_core_strbuf_sets(&parser->linebuf, markdown_core_strbuf_cstr(&saved_linebuf));
    markdown_core_strbuf_free(&saved_linebuf);
}

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof) {
    const unsigned char *end = buffer + len;
    static const uint8_t repl[] = {239, 191, 189};

    if (len > UINT_MAX - parser->total_size)
        parser->total_size = UINT_MAX;
    else
        parser->total_size += len;

    if (parser->last_buffer_ended_with_cr && *buffer == '\n') {
        // skip NL if last buffer ended with CR ; see #117
        buffer++;
    }
    parser->last_buffer_ended_with_cr = false;
    while (buffer < end) {
        const unsigned char *eol;
        bufsize_t chunk_len;
        bool process = false;
        for (eol = buffer; eol < end; ++eol) {
            if (S_is_line_end_char(*eol)) {
                process = true;
                break;
            }
            if (*eol == '\0' && eol < end) {
                break;
            }
        }
        if (eol >= end && eof) {
            process = true;
        }

        chunk_len = (bufsize_t)(eol - buffer);
        if (process) {
            if (parser->linebuf.size > 0) {
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
                markdown_core_strbuf_clear(&parser->linebuf);
            } else {
                S_process_line(parser, buffer, chunk_len);
            }
        } else {
            if (eol < end && *eol == '\0') {
                // omit NULL byte
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                // add replacement character
                markdown_core_strbuf_put(&parser->linebuf, repl, 3);
            } else {
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
            }
        }

        buffer += chunk_len;
        if (buffer < end) {
            if (*buffer == '\0') {
                // skip over NULL
                buffer++;
            } else {
                // skip over line ending characters
                if (*buffer == '\r') {
                    buffer++;
                    if (buffer == end)
                        parser->last_buffer_ended_with_cr = true;
                }
                if (buffer < end && *buffer == '\n')
                    buffer++;
            }
        }
    }
}

static void chop_trailing_hashtags(markdown_core_chunk *ch) {
    bufsize_t n, orig_n;

    markdown_core_chunk_rtrim(ch);
    orig_n = n = ch->len - 1;

    // if string ends in space followed by #s, remove these:
    while (n >= 0 && peek_at(ch, n) == '#')
        n--;

    // Check for a space before the final #s:
    if (n != orig_n && n >= 0 && S_is_space_or_tab(peek_at(ch, n))) {
        ch->len = n;
        markdown_core_chunk_rtrim(ch);
    }
}

// Check for thematic break.  On failure, return 0 and update
// thematic_break_kill_pos with the index at which the
// parse fails.  On success, return length of match.
// "...three or more hyphens, asterisks,
// or underscores on a line by themselves. If you wish, you may use
// spaces between the hyphens or asterisks."
static int S_scan_thematic_break(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t offset) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = peek_at(input, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = peek_at(input, ++i))) {
        if (nextc == c) {
            count++;
        } else if (nextc != ' ' && nextc != '\t') {
            break;
        }
    }
    if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
        return (i - offset) + 1;
    } else {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
}

// Find first nonspace character from current offset, setting
// parser->first_nonspace, parser->first_nonspace_column,
// parser->indent, and parser->blank. Does not advance parser->offset.
static void S_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input) {
    char c;
    int chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);

    if (parser->first_nonspace <= parser->offset) {
        parser->first_nonspace = parser->offset;
        parser->first_nonspace_column = parser->column;
        while ((c = peek_at(input, parser->first_nonspace))) {
            if (c == ' ') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += 1;
                chars_to_tab = chars_to_tab - 1;
                if (chars_to_tab == 0) {
                    chars_to_tab = TAB_STOP;
                }
            } else if (c == '\t') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += chars_to_tab;
                chars_to_tab = TAB_STOP;
            } else {
                break;
            }
        }
    }

    parser->indent = parser->first_nonspace_column - parser->column;
    parser->blank = S_is_line_end_char(peek_at(input, parser->first_nonspace));
}

// Advance parser->offset and parser->column.  parser->offset is the
// byte position in input; parser->column is a virtual column number
// that takes into account tabs. (Multibyte characters are not taken
// into account, because the Markdown line prefixes we are interested in
// analyzing are entirely ASCII.)  The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->partially_consumed_tab is set to true.
static void S_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count, bool columns) {
    char c;
    int chars_to_tab;
    int chars_to_advance;
    while (count > 0 && (c = peek_at(input, parser->offset))) {
        if (c == '\t') {
            chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
            if (columns) {
                parser->partially_consumed_tab = chars_to_tab > count;
                chars_to_advance = MIN(count, chars_to_tab);
                parser->column += chars_to_advance;
                parser->offset += (parser->partially_consumed_tab ? 0 : 1);
                count -= chars_to_advance;
            } else {
                parser->partially_consumed_tab = false;
                parser->column += chars_to_tab;
                parser->offset += 1;
                count -= 1;
            }
        } else {
            parser->partially_consumed_tab = false;
            parser->offset += 1;
            parser->column += 1; // assume ascii; block starts are ascii
            count -= 1;
        }
    }
}

static bool S_last_child_is_open(markdown_core_node *container) {
    return container->last_child && (container->last_child->flags & MARKDOWN_CORE_NODE__OPEN);
}

static bool parse_block_quote_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->indent <= 3 && peek_at(input, parser->first_nonspace) == '>';
    if (matched) {

        S_advance_offset(parser, input, parser->indent + 1, true);

        if (S_is_space_or_tab(peek_at(input, parser->offset))) {
            S_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool parse_footnote_definition_block_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                                   markdown_core_node *container) {
    if (parser->indent >= 4) {
        S_advance_offset(parser, input, 4, true);
        return true;
    } else if (input->len > 0 && (input->data[0] == '\n' || (input->data[0] == '\r' && input->data[1] == '\n'))) {
        return true;
    }

    return false;
}

static bool parse_node_item_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                   markdown_core_node *container) {
    bool res = false;

    if (parser->indent >= container->as.list.marker_offset + container->as.list.padding) {
        S_advance_offset(parser, input, container->as.list.marker_offset + container->as.list.padding, true);
        res = true;
    } else if (parser->blank && container->first_child != NULL) {
        // if container->first_child is NULL, then the opening line
        // of the list item was blank after the list marker; in this
        // case, we are done with the list item.
        S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        res = true;
    }
    return res;
}

static bool parse_code_block_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                    markdown_core_node *container, bool *should_continue) {
    bool res = false;

    if (!container->as.code.fenced) { // indented
        if (parser->indent >= CODE_INDENT) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            res = true;
        } else if (parser->blank) {
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) == container->as.code.fence_char)) {
            matched = scan_close_code_fence(input, parser->first_nonspace);
        }

        if (matched >= container->as.code.fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            *should_continue = false;
            container->as.code.fence_closed = true;
            S_advance_offset(parser, input, matched, false);
            parser->current = finalize(parser, container);
        } else {
            // skip opt. spaces of fence parser->offset
            int i = container->as.code.fence_offset;

            while (i > 0 && S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

static bool parse_html_block_prefix(markdown_core_parser *parser, markdown_core_node *container) {
    bool res = false;
    int html_block_type = container->as.html_block_type;

    assert(html_block_type >= 1 && html_block_type <= 7);
    switch (html_block_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        // these types of blocks can accept blanks
        res = true;
        break;
    case 6:
    case 7:
        res = !parser->blank;
        break;
    }

    return res;
}

static bool parse_extension_block(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_chunk *input) {
    bool res = false;

    if (container->extension->last_block_matches) {
        if (container->extension->last_block_matches(container->extension, parser, input->data, input->len, container))
            res = true;
    }

    return res;
}

/**
 * For each containing node, try to parse the associated line start.
 *
 * Will not close unmatched blocks, as we may have a lazy continuation
 * line -> http://spec.commonmark.org/0.24/#lazy-continuation-line
 *
 * Returns: The last matching node, or NULL
 */
static markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                             bool *all_matched) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = parser->root;
    markdown_core_node_type cont_type;

    while (S_last_child_is_open(container)) {
        container = container->last_child;
        cont_type = S_type(container);

        S_find_first_nonspace(parser, input);

        if (container->extension) {
            if (!parse_extension_block(parser, container, input))
                goto done;
            continue;
        }

        switch (cont_type) {
        case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
            if (!parse_block_quote_prefix(parser, input))
                goto done;
            break;
        case MARKDOWN_CORE_NODE_LIST_ITEM:
            if (!parse_node_item_prefix(parser, input, container))
                goto done;
            break;
        case MARKDOWN_CORE_NODE_CODE_BLOCK:
            if (!parse_code_block_prefix(parser, input, container, &should_continue))
                goto done;
            break;
        case MARKDOWN_CORE_NODE_HEADING:
            // a heading can never contain more than one line
            goto done;
        case MARKDOWN_CORE_NODE_HTML_BLOCK:
            if (!parse_html_block_prefix(parser, container))
                goto done;
            break;
        case MARKDOWN_CORE_NODE_PARAGRAPH:
            if (parser->blank)
                goto done;
            break;
        case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
            if (!parse_footnote_definition_block_prefix(parser, input, container))
                goto done;
            break;
        default:
            break;
        }
    }

    *all_matched = true;

done:
    if (!*all_matched) {
        container = container->parent; // back up to last matching node
    }

    if (!should_continue) {
        container = NULL;
    }

    return container;
}

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
    bool indented;
    markdown_core_list *data = NULL;
    bool maybe_lazy = S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH;
    markdown_core_node_type cont_type = S_type(*container);
    bufsize_t matched = 0;
    int lev = 0;
    bool save_partially_consumed_tab;
    bool has_content;
    int save_offset;
    int save_column;
    size_t depth = 0;

    while (cont_type != MARKDOWN_CORE_NODE_CODE_BLOCK && cont_type != MARKDOWN_CORE_NODE_HTML_BLOCK &&
           !extension_accepts_lines(*container)) {
        depth++;
        S_find_first_nonspace(parser, input);
        indented = parser->indent >= CODE_INDENT;

        if (!indented && peek_at(input, parser->first_nonspace) == '>') {

            bufsize_t blockquote_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
            // optional following character
            if (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_BLOCK_QUOTE, blockquote_startpos + 1);
            if (!*container)
                return;

        } else if (!indented && (matched = scan_atx_heading_start(input, parser->first_nonspace))) {
            bufsize_t hashpos;
            int level = 0;
            bufsize_t heading_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
            if (!*container)
                return;

            hashpos = markdown_core_chunk_strchr(input, '#', parser->first_nonspace);

            while (peek_at(input, hashpos) == '#') {
                level++;
                hashpos++;
            }

            (*container)->as.heading.level = level;
            (*container)->as.heading.setext = false;
            (*container)->internal_offset = matched;

        } else if (!indented && (matched = scan_open_code_fence(input, parser->first_nonspace))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->first_nonspace + 1);
            if (!*container)
                return;
            (*container)->as.code.fenced = true;
            (*container)->as.code.fence_char = peek_at(input, parser->first_nonspace);
            (*container)->as.code.fence_length = (matched > 255) ? 255 : (uint8_t)matched;
            (*container)->as.code.fence_offset = (int8_t)(parser->first_nonspace - parser->offset);
            (*container)->as.code.fence_closed = false;
            (*container)->as.code.info = markdown_core_chunk_literal("");
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

        } else if (!indented && ((matched = scan_html_block_start(input, parser->first_nonspace)) ||
                                 (cont_type != MARKDOWN_CORE_NODE_PARAGRAPH &&
                                  (matched = scan_html_block_start_7(input, parser->first_nonspace))))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, parser->first_nonspace + 1);
            if (!*container)
                return;
            (*container)->as.html_block_type = matched;
            // note, we don't adjust parser->offset because the tag is part of the
            // text
        } else if (!indented && cont_type == MARKDOWN_CORE_NODE_PARAGRAPH &&
                   (lev = scan_setext_heading_line(input, parser->first_nonspace))) {
            // finalize paragraph, resolving reference links
            has_content = resolve_reference_link_definitions(parser, *container);

            if (has_content) {

                (*container)->type = (uint16_t)MARKDOWN_CORE_NODE_HEADING;
                (*container)->as.heading.level = lev;
                (*container)->as.heading.setext = true;
                S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
            }
        } else if (!indented && !(cont_type == MARKDOWN_CORE_NODE_PARAGRAPH && !all_matched) &&
                   (parser->thematic_break_kill_pos <= parser->first_nonspace) &&
                   (matched = S_scan_thematic_break(parser, input, parser->first_nonspace))) {
            // it's only now that we know the line is not part of a setext heading:
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK, parser->first_nonspace + 1);
            if (!*container)
                return;
            S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
        } else if (!indented && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && depth < MAX_LIST_DEPTH &&
                   (matched = scan_footnote_definition(input, parser->first_nonspace))) {
            markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);

            while (c.data[c.len - 1] != ']')
                --c.len;
            --c.len;

            if (!markdown_core_chunk_to_cstr(parser->mem, &c)) {
                /* The label would keep borrowing the transient line buffer. */
                parser->oom = true;
                return;
            }

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION,
                                   parser->first_nonspace + matched + 1);
            if (!*container) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            (*container)->as.literal = c;

            (*container)->internal_offset = matched;
        } else if ((!indented || cont_type == MARKDOWN_CORE_NODE_LIST) && parser->indent < 4 &&
                   depth < MAX_LIST_DEPTH &&
                   (matched = parse_list_marker(parser, input, parser->first_nonspace,
                                                (*container)->type == MARKDOWN_CORE_NODE_PARAGRAPH, &data))) {

            // Note that we can have new list items starting with >= 4
            // spaces indent, as long as the list container is still open.
            int i = 0;

            // compute padding:
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

            save_partially_consumed_tab = parser->partially_consumed_tab;
            save_offset = parser->offset;
            save_column = parser->column;

            while (parser->column - save_column <= 5 && S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }

            i = parser->column - save_column;
            if (i >= 5 || i < 1 ||
                // only spaces after list marker:
                S_is_line_end_char(peek_at(input, parser->offset))) {
                data->padding = matched + 1;
                parser->offset = save_offset;
                parser->column = save_column;
                parser->partially_consumed_tab = save_partially_consumed_tab;
                if (i > 0) {
                    S_advance_offset(parser, input, 1, true);
                }
            } else {
                data->padding = matched + i;
            }

            // check container; if it's a list, see if this list item
            // can continue the list; otherwise, create a list container.

            data->marker_offset = parser->indent;

            if (cont_type != MARKDOWN_CORE_NODE_LIST || !lists_match(&((*container)->as.list), data)) {
                *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, parser->first_nonspace + 1);
                if (!*container) {
                    parser->mem->free(data);
                    return;
                }

                memcpy(&((*container)->as.list), data, sizeof(*data));
            }

            // add the list item
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, parser->first_nonspace + 1);
            if (!*container) {
                parser->mem->free(data);
                return;
            }
            memcpy(&((*container)->as.list), data, sizeof(*data));
            parser->mem->free(data);
        } else if (indented && !maybe_lazy && !parser->blank) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->offset + 1);
            if (!*container)
                return;
            (*container)->as.code.fenced = false;
            (*container)->as.code.fence_char = 0;
            (*container)->as.code.fence_length = 0;
            (*container)->as.code.fence_offset = 0;
            (*container)->as.code.fence_closed = false;
            (*container)->as.code.info = markdown_core_chunk_literal("");
        } else {
            markdown_core_llist *tmp;
            markdown_core_node *new_container = NULL;

            for (tmp = parser->syntax_extensions; tmp; tmp = tmp->next) {
                markdown_core_syntax_extension *ext = (markdown_core_syntax_extension *)tmp->data;

                if (ext->try_opening_block) {
                    new_container = ext->try_opening_block(ext, indented, parser, *container, input->data, input->len);

                    if (new_container) {
                        *container = new_container;
                        break;
                    }
                }
            }

            if (!new_container) {
                break;
            }
        }

        if (accepts_lines(*container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        cont_type = S_type(*container);
        maybe_lazy = false;
    }
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    S_find_first_nonspace(parser, input);

    if (parser->blank && container->last_child)
        S_set_last_line_blank(container->last_child, true);

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_node_type ctype = S_type(container);
    const bool last_line_blank =
        (parser->blank && ctype != MARKDOWN_CORE_NODE_BLOCK_QUOTE && ctype != MARKDOWN_CORE_NODE_HEADING &&
         ctype != MARKDOWN_CORE_NODE_THEMATIC_BREAK && !extension_accepts_lines(container) &&
         !(ctype == MARKDOWN_CORE_NODE_CODE_BLOCK && container->as.code.fenced) &&
         !(ctype == MARKDOWN_CORE_NODE_LIST_ITEM && container->first_child == NULL &&
           container->start_line == parser->line_number));

    S_set_last_line_blank(container, last_line_blank);

    tmp = container;
    while (tmp->parent) {
        S_set_last_line_blank(tmp->parent, false);
        tmp = tmp->parent;
    }

    // If the last line processed belonged to a paragraph node,
    // and we didn't match all of the line prefixes for the open containers,
    // and we didn't start any new containers,
    // and the line isn't blank,
    // then treat this as a "lazy continuation line" and add it to
    // the open paragraph.
    if (parser->current != last_matched_container && container == last_matched_container && !parser->blank &&
        S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH) {
        add_line(parser->current, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        while (parser->current != last_matched_container) {
            parser->current = finalize(parser, parser->current);
            assert(parser->current != NULL);
        }

        if (S_type(container) == MARKDOWN_CORE_NODE_CODE_BLOCK) {
            add_line(container, input, parser);
        } else if (S_type(container) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
            add_line(container, input, parser);

            int matches_end_condition;
            switch (container->as.html_block_type) {
            case 1:
                // </script>, </style>, </pre>
                matches_end_condition = scan_html_block_end_1(input, parser->first_nonspace);
                break;
            case 2:
                // -->
                matches_end_condition = scan_html_block_end_2(input, parser->first_nonspace);
                break;
            case 3:
                // ?>
                matches_end_condition = scan_html_block_end_3(input, parser->first_nonspace);
                break;
            case 4:
                // >
                matches_end_condition = scan_html_block_end_4(input, parser->first_nonspace);
                break;
            case 5:
                // ]]>
                matches_end_condition = scan_html_block_end_5(input, parser->first_nonspace);
                break;
            default:
                matches_end_condition = 0;
                break;
            }

            if (matches_end_condition) {
                container = finalize(parser, container);
                assert(parser->current != NULL);
            }
        } else if (extension_accepts_lines(container)) {
            add_line(container, input, parser);
        } else if (parser->blank) {
            // ??? do nothing
        } else if (accepts_lines(container)) {
            if (S_type(container) == MARKDOWN_CORE_NODE_HEADING && container->as.heading.setext == false) {
                chop_trailing_hashtags(input);
            }
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        } else {
            // create paragraph container for line
            container = add_child(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, parser->first_nonspace + 1);
            if (!container)
                return;
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        }

        parser->current = container;
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;
    markdown_core_node *current;

    if (parser->oom || parser->root == NULL)
        return;

    markdown_core_strbuf_clear(&parser->curline);

    if (parser->options & MARKDOWN_CORE_OPT_VALIDATE_UTF8)
        markdown_core_utf8proc_check(&parser->curline, buffer, bytes);
    else
        markdown_core_strbuf_put(&parser->curline, buffer, bytes);

    bytes = parser->curline.size;

    // ensure line ends with a newline:
    if (bytes == 0 || !S_is_line_end_char(parser->curline.ptr[bytes - 1]))
        markdown_core_strbuf_putc(&parser->curline, '\n');

    if (parser->curline.oom) {
        parser->oom = true;
        return;
    }

    parser->offset = 0;
    parser->column = 0;
    parser->first_nonspace = 0;
    parser->first_nonspace_column = 0;
    parser->thematic_break_kill_pos = 0;
    parser->indent = 0;
    parser->blank = false;
    parser->partially_consumed_tab = false;

    input.data = parser->curline.ptr;
    input.len = parser->curline.size;
    input.alloc = 0;

    // Skip UTF-8 BOM.
    if (parser->line_number == 0 && input.len >= 3 && memcmp(input.data, "\xef\xbb\xbf", 3) == 0)
        parser->offset += 3;

    parser->line_number++;

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container)
        goto finished;

    container = last_matched_container;

    current = parser->current;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->oom)
        goto finished;

    /* parser->current might have changed if feed_reentrant was called */
    if (current == parser->current)
        add_text_to_container(parser, container, last_matched_container, &input);

finished:
    parser->last_line_length = input.len;
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\n')
        parser->last_line_length -= 1;
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\r')
        parser->last_line_length -= 1;

    markdown_core_strbuf_clear(&parser->curline);
}

markdown_core_node *markdown_core_parser_finish(markdown_core_parser *parser) {
    markdown_core_node *res;
    markdown_core_llist *extensions;

    /* Parser was already finished once */
    if (parser->root == NULL)
        return NULL;

    if (parser->linebuf.size) {
        S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
        markdown_core_strbuf_clear(&parser->linebuf);
    }

    finalize_document(parser);

    if (!markdown_core_consolidate_text_nodes(parser->root))
        parser->oom = true;

    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);

#if MARKDOWN_CORE_DEBUG_NODES
    if (markdown_core_node_check(parser->root, stderr)) {
        abort();
    }
#endif

    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
        markdown_core_syntax_extension *ext = (markdown_core_syntax_extension *)extensions->data;
        if (ext->postprocess_func) {
            markdown_core_node *processed = ext->postprocess_func(ext, parser, parser->root);
            if (processed)
                parser->root = processed;
        }
    }

    if (parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) {
        if (!S_strip_html_comments(parser->root))
            parser->oom = true;
    }

    /* All allocation-loss routes converge here: block/inline structures set
     * parser->oom directly, definition maps carry their own sticky flag. */
    if (parser->refmap && parser->refmap->oom)
        parser->oom = true;
    if (parser->oom) {
        markdown_core_node_free(parser->root);
        parser->root = NULL;
        markdown_core_parser_reset(parser);
        return NULL;
    }

    res = parser->root;
    parser->root = NULL;

    markdown_core_parser_reset(parser);

    return res;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->offset; }

bufsize_t markdown_core_parser_get_column(markdown_core_parser *parser) { return parser->column; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->first_nonspace; }

int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser) {
    return parser->first_nonspace_column;
}

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->blank; }

int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser) {
    return parser->partially_consumed_tab;
}

int markdown_core_parser_get_last_line_length(markdown_core_parser *parser) { return parser->last_line_length; }

markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    return add_child(parser, parent, block_type, start_column);
}

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    S_advance_offset(parser, &input_chunk, count, columns != 0);
}

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}

markdown_core_llist *markdown_core_parser_get_syntax_extensions(markdown_core_parser *parser) {
    return parser->syntax_extensions;
}
