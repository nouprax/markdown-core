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
#include <stdint.h>
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

    if (!iter) {
        return false;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        /* EXIT, not ENTER: the mutation rule names the node whose EXIT is
         * current, and it is the only moment the iterator's lookahead is
         * outside this node's subtree. `HTML` and `HTML_BLOCK` were both in
         * the old `S_is_leaf` list, so their EXIT was suppressed and freeing
         * at ENTER happened to be safe; with the contract total it is a
         * use-after-free on the very next `markdown_core_iter_next`. */
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && S_html_literal_starts_with_comment(node)) {
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
    if (is_blank) {
        node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    } else {
        node->flags &= ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    }
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
    if (!e) {
        return NULL;
    }
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
 * silent-drop behavior.
 *
 * Both extension lists hold pointers to the `static const` descriptors that
 * Step 3b made read-only, and every reader casts `data` straight back to a
 * `const markdown_core_syntax_extension *`. The const is discarded here and
 * nowhere else because markdown_core_llist is a generic list that cannot
 * carry it; typing the parameter keeps the cast to this one line. */
static int S_extension_list_append(markdown_core_mem *mem, markdown_core_llist **head,
                                   const markdown_core_syntax_extension *extension) {
    markdown_core_llist *node = (markdown_core_llist *)mem->calloc(1, sizeof(*node));
    markdown_core_llist *tail;
    if (!node) {
        return 0;
    }
    node->data = (void *)(uintptr_t)extension;
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
                                                 const markdown_core_syntax_extension *extension) {
    if (!S_extension_list_append(parser->mem, &parser->syntax_extensions, extension)) {
        return 0;
    }
    if (extension->match_inline || extension->insert_inline_from_delim) {
        if (!S_extension_list_append(parser->mem, &parser->inline_syntax_extensions, extension)) {
            return 0;
        }
    }

    return 1;
}

static void markdown_core_parser_dispose(markdown_core_parser *parser) {
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    if (parser->refmap) {
        markdown_core_map_free(parser->refmap);
    }

    /* The definition set holds labels and no nodes, so freeing it here cannot
     * reach the tree -- which is the whole difference between it and the map
     * `process_footnotes` used to build (D11). */
    if (parser->footnote_defs) {
        markdown_core_map_free(parser->footnote_defs);
        parser->footnote_defs = NULL;
    }

    /* The normalized source and its line index are per-parse and are released
     * with the rest of it. Requirement 12 is where a document keeps them.
     *
     * `mem` guards the first call: `markdown_core_parser_new_with_mem` reaches
     * here through `reset` on a calloc'd parser, and strbuf_free dereferences
     * the buffer's own allocator. */
    if (parser->source.mem) {
        markdown_core_strbuf_free(&parser->source);
    }
    parser->mem->free(parser->line_starts);
    parser->line_starts = NULL;
    parser->line_starts_size = 0;
    parser->line_starts_alloc = 0;
    parser->mem->free(parser->regions);
    parser->regions = NULL;
    parser->regions_size = 0;
    parser->regions_alloc = 0;
    parser->region_cursor = 0;

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    parser->mem->free(parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;

    /* Requirement 11b's scratch. Both are reused block to block and neither
     * outlives the parse -- the claims are resolved into regions as each block
     * finishes, and the paint is only ever read within the block that filled
     * it. */
    parser->mem->free(parser->inline_claims);
    parser->inline_claims = NULL;
    parser->inline_claims_size = 0;
    parser->inline_claims_alloc = 0;
    parser->mem->free(parser->inline_paint);
    parser->inline_paint = NULL;
    parser->inline_paint_alloc = 0;
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
    markdown_core_strbuf_init(parser->mem, &parser->source, 0);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;

    parser->syntax_extensions = saved_exts;
    parser->inline_syntax_extensions = saved_inline_exts;
    parser->options = saved_options;

    /* A reset that could not rebuild its structures poisons the parser: feed
     * becomes a no-op and finish reports failure. */
    if (!parser->root || !parser->refmap || !parser->footnote_defs || parser->curline.oom || parser->source.oom) {
        parser->oom = true;
    }

    markdown_core_inlines_reset_special_chars(parser);
}

markdown_core_parser *markdown_core_parser_new_with_mem(int options, markdown_core_mem *mem) {
    markdown_core_parser *parser = (markdown_core_parser *)mem->calloc(1, sizeof(markdown_core_parser));
    if (!parser) {
        return NULL;
    }
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
    markdown_core_strbuf_free(&parser->source);
    markdown_core_llist_free(parser->mem, parser->syntax_extensions);
    markdown_core_llist_free(parser->mem, parser->inline_syntax_extensions);
    mem->free(parser);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b);

/* "This block ends on the line being processed", lifted out of `finalize` so
 * that the extension close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An extension container closing on its own fence is a fourth,
 * and `finalize` cannot know that from the type alone. */
static void S_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
    b->end_line = parser->line_number;
    b->end_column = parser->curline.size;
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n') {
        b->end_column -= 1;
    }
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r') {
        b->end_column -= 1;
    }
}

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

    if (extension_accepts_lines(node)) {
        return true;
    }

    return (block_type == MARKDOWN_CORE_NODE_PARAGRAPH || block_type == MARKDOWN_CORE_NODE_HEADING ||
            block_type == MARKDOWN_CORE_NODE_CODE_BLOCK);
}

static MARKDOWN_CORE_INLINE bool contains_inlines(markdown_core_node *node) {
    if (node->extension && node->extension->contains_inlines_func) {
        return node->extension->contains_inlines_func(node->extension, node) != 0;
    }

    return (node->type == MARKDOWN_CORE_NODE_PARAGRAPH || node->type == MARKDOWN_CORE_NODE_HEADING);
}

/* Attribute the bytes of the line in hand up to `upto` -- a LINE offset -- to
 * `owner` in role `role`, and move the cursor there.
 *
 * The cursor is what makes the tiling hold by construction rather than by
 * check: regions are laid down left to right with no gap and no overlap,
 * because every one of them starts where the last one ended. A caller that
 * claims backwards claims nothing; a byte no caller claims is swept at the end
 * of the line. That is L1 and L3 together, and neither needs an oracle.
 *
 * A zero-length claim is not a region. A claim contiguous with the previous
 * region, under the same owner and role, extends it instead of adding one --
 * the tiling is a partition of bytes, not a record of how many times the
 * parser looked. */
static void S_claim_region(markdown_core_parser *parser, markdown_core_node *owner, bufsize_t upto,
                           markdown_core_region_role role) {
    markdown_core_region *region;
    bufsize_t base;

    if (!owner || parser->line_starts_size == 0 || upto <= parser->region_cursor) {
        return;
    }
    base = parser->line_starts[parser->line_starts_size - 1];

    if (parser->regions_size > 0) {
        region = &parser->regions[parser->regions_size - 1];
        if (region->owner == owner && region->role == (uint8_t)role &&
            region->start + region->length == base + parser->region_cursor) {
            region->length += upto - parser->region_cursor;
            parser->region_cursor = upto;
            return;
        }
    }

    if (parser->regions_size == parser->regions_alloc) {
        bufsize_t alloc = parser->regions_alloc ? parser->regions_alloc * 2 : 256;
        markdown_core_region *grown;
        if (parser->regions_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return;
        }
        grown = (markdown_core_region *)parser->mem->realloc(parser->regions, (size_t)alloc * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->regions = grown;
        parser->regions_alloc = alloc;
    }

    region = &parser->regions[parser->regions_size++];
    region->start = base + parser->region_cursor;
    region->length = upto - parser->region_cursor;
    region->owner = owner;
    region->role = (uint8_t)role;
    parser->region_cursor = upto;
}

/* Hand every region `node` owns to `node->parent`, in role DISCARDED.
 *
 * Called where a block that has already claimed bytes is freed -- a paragraph
 * that turned out to be nothing but reference definitions. The bytes do not
 * stop existing when the node does, and a region naming a freed node is the
 * same defect as a map that owns a node (D11), one indirection further out.
 *
 * Bounded by the block, not by the document: regions are appended in source
 * order, so nothing before the block's own first line can name it. */
static void S_reown_regions(markdown_core_parser *parser, markdown_core_node *from_node, markdown_core_node *to_node,
                            int from_line, int upto_line, bool discard) {
    bufsize_t from = 0;
    bufsize_t stop;
    bufsize_t i;

    if (!parser || !from_node || !to_node || from_node == to_node || from_line <= 0) {
        return;
    }
    if (from_line <= parser->line_starts_size) {
        bufsize_t lo = 0;
        bufsize_t hi = parser->regions_size;
        bufsize_t want = parser->line_starts[from_line - 1];
        while (lo < hi) {
            bufsize_t mid = lo + (hi - lo) / 2;
            if (parser->regions[mid].start < want) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        from = lo;
    }
    /* `upto_line` bounds the scan ABOVE, which a caller replacing one node with
     * another can do and a caller giving a block's bytes up cannot: a node owns
     * no region past its own last line, so a replacement costs the node's own
     * lines rather than every region after them. Consolidation replaces one
     * text node per merge and would otherwise be quadratic in the document. */
    stop = parser->regions_size;
    if (upto_line > 0 && upto_line < parser->line_starts_size) {
        bufsize_t lo = from;
        bufsize_t hi = parser->regions_size;
        bufsize_t want = parser->line_starts[upto_line];
        while (lo < hi) {
            bufsize_t mid = lo + (hi - lo) / 2;
            if (parser->regions[mid].start < want) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        stop = lo;
    }
    for (i = from; i < stop; i++) {
        if (parser->regions[i].owner == from_node) {
            parser->regions[i].owner = to_node;
            if (discard) {
                parser->regions[i].role = (uint8_t)MARKDOWN_CORE_REGION_DISCARDED;
            }
        }
    }
}

static void S_disown_regions(markdown_core_parser *parser, markdown_core_node *node, int from_line) {
    if (!node) {
        return;
    }
    S_reown_regions(parser, node, node->parent ? node->parent : parser->root, from_line, 0, true);
}

/* `to` takes the regions `from` owns, with a FORWARD-ONLY CURSOR shared across
 * a run of such calls.
 *
 * Consolidation merges a run of adjacent text nodes into its first, and those
 * nodes are in source order, so their regions are too. Scanning from the top
 * for each one is quadratic and it is not hypothetical: 300,000 `<!--` in one
 * paragraph is 600,000 text nodes on ONE LINE, which
 * `markdown_core_parser_transfer_regions`' line bound does not narrow at all --
 * measured as a TIMEOUT in `pathological_unclosed_comment`. A cursor that only
 * moves forward makes the whole run cost the run.
 *
 * `*cursor` is initialised from `from`'s own start on the first call; the
 * caller passes -1 to ask for that. */
void markdown_core_parser_absorb_regions(markdown_core_parser *parser, markdown_core_node *from, markdown_core_node *to,
                                         bufsize_t *cursor) {
    bufsize_t stop;

    if (!parser || !from || !to || from == to || !cursor) {
        return;
    }
    if (from->start_line < 1 || from->start_line > parser->line_starts_size || from->end_line < 1 ||
        from->end_line > parser->line_starts_size) {
        return;
    }
    if (*cursor < 0) {
        bufsize_t lo = 0;
        bufsize_t hi = parser->regions_size;
        bufsize_t want = parser->line_starts[from->start_line - 1] + from->start_column - 1;
        while (lo < hi) {
            bufsize_t mid = lo + (hi - lo) / 2;
            if (parser->regions[mid].start + parser->regions[mid].length <= want) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        *cursor = lo;
    }
    stop = parser->line_starts[from->end_line - 1] + from->end_column;
    while (*cursor < parser->regions_size && parser->regions[*cursor].start < stop) {
        if (parser->regions[*cursor].owner == from) {
            parser->regions[*cursor].owner = to;
        }
        (*cursor)++;
    }
}

void markdown_core_parser_transfer_regions(markdown_core_parser *parser, markdown_core_node *from,
                                           markdown_core_node *to) {
    if (!from || !to) {
        return;
    }
    /* The roles are kept: this is one node taking another's place, not bytes
     * being given up. `from->start_line` bounds the scan to the span the node
     * could have claimed in, so a replacement costs the block and not the
     * document. */
    S_reown_regions(parser, from, to, from->start_line, from->end_line, false);
}

/* Write the concrete record set in the form the gate reads.
 *
 * One `line` row per line and one `region` row per region, both in source
 * order, plus the owner's tree path and scope so the gate can check L2 without
 * a second parse. The record set has no public reader yet -- requirement 12 is
 * where a document keeps it -- so this exists for the gate and says so. */
static void S_write_node_path(FILE *out, markdown_core_node *node, markdown_core_node *root) {
    int index = 0;
    markdown_core_node *sibling;

    if (node == root || !node->parent) {
        fputc('0', out);
        return;
    }
    S_write_node_path(out, node->parent, root);
    for (sibling = node->parent->first_child; sibling && sibling != node; sibling = sibling->next) {
        index++;
    }
    fprintf(out, ".%d", index);
}

static void S_write_concrete(markdown_core_parser *parser, FILE *out) {
    static const char *const ROLE[] = {"MARKER", "CONTENT", "DISCARDED"};
    bufsize_t i;

    fprintf(out, "concrete source=%ld lines=%ld regions=%ld\n", (long)parser->source.size,
            (long)parser->line_starts_size, (long)parser->regions_size);
    for (i = 0; i < parser->line_starts_size; i++) {
        fprintf(out, "line %ld %ld\n", (long)i + 1, (long)parser->line_starts[i]);
    }
    for (i = 0; i < parser->regions_size; i++) {
        const markdown_core_region *region = &parser->regions[i];
        fprintf(out, "region %ld %ld %s ", (long)region->start, (long)region->length, ROLE[region->role]);
        S_write_node_path(out, region->owner, parser->root);
        fprintf(out, " %s %d:%d..%d:%d\n", markdown_core_node_get_type_string(region->owner), region->owner->start_line,
                region->owner->start_column, region->owner->end_line, region->owner->end_column);
    }
    /* EVERY NODE, in preorder, whether or not it owns a region.
     *
     * A record set that names no inline node at all satisfies "every byte is
     * owned by an inline node OR BY THE BLOCK" -- so the law alone cannot tell
     * requirement 11b from the day before it. What separates them is coverage:
     * an inline node's scope is exactly the bytes it and its descendants own,
     * and a checker cannot say that about a node it never sees. These rows are
     * how it sees them, and they are the RAW tree's -- the same traversal
     * `S_write_node_path` walks, not the facade's, which hides the directive
     * label wrapper. */
    {
        markdown_core_iter *iter = markdown_core_iter_new(parser->root);
        markdown_core_event_type event;
        if (iter) {
            while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
                markdown_core_node *node;
                if (event != MARKDOWN_CORE_EVENT_ENTER) {
                    continue;
                }
                node = markdown_core_iter_get_node(iter);
                fprintf(out, "node ");
                S_write_node_path(out, node, parser->root);
                fprintf(out, " %s %d:%d..%d:%d\n", markdown_core_node_get_type_string(node), node->start_line,
                        node->start_column, node->end_line, node->end_column);
            }
            markdown_core_iter_free(iter);
        }
    }
}

#define MARKDOWN_CORE_MAX_INLINE_DEPTH 256

/* A region 11b may refine: CONTENT, and owned by the block whose inlines were
 * just parsed OR BY AN ANCESTOR OF IT.
 *
 * The ancestor case is not a liberty; it is the only way two kinds of block
 * reach their own bytes at all. A table CELL and a directive LABEL have their
 * content SET rather than fed, so the block-level regions covering those bytes
 * belong to the TABLE and to the DIRECTIVE. Refining one of theirs into the
 * cell's own inline nodes is a refinement -- the same bytes, a descendant
 * owner -- and refusing it leaves every node inside a table or a label owning
 * nothing at all, which is what L5 says out loud. */
static int S_region_is_refinable(const markdown_core_region *region, markdown_core_node *b) {
    markdown_core_node *node;
    if (region->role != (uint8_t)MARKDOWN_CORE_REGION_CONTENT) {
        return 0;
    }
    /* The owner must be the block itself or an ANCESTOR of it. Anything else
     * with role CONTENT is a region a NESTED refine already wrote -- a
     * directive's label is inline-parsed from inside its own paragraph's
     * pass -- and refining it twice overlaps, which L1 says out loud. */
    for (node = b; node; node = node->parent) {
        if (region->owner == node) {
            return 1;
        }
    }
    return 0;
}

static int S_compare_node_pointers(const void *a, const void *b) {
    markdown_core_node *const *left = (markdown_core_node *const *)a;
    markdown_core_node *const *right = (markdown_core_node *const *)b;
    if (*left < *right) {
        return -1;
    }
    return *left > *right ? 1 : 0;
}

/* L2's first clause, made an invariant AFTER the last thing that moves an
 * owner. `markdown_core_parser_refine_inline_regions` already refuses to give a
 * piece to a node whose scope does not contain it, but consolidation runs later
 * and moves regions onto a surviving text node whose own end deliberately stops
 * at its last operand that OWNS BYTES -- so an empty operand's bytes can end up
 * on a node that does not reach them. Those bytes go back to the block, in role
 * CONTENT, which is always true of them.
 *
 * Block owners are left alone: their out-of-scope rows are the registered
 * families in `specs/concrete/records.json`, and sweeping them here would hide
 * the defects rather than fix them. */
static void S_reseat_inline_regions(markdown_core_parser *parser) {
    markdown_core_node **live = NULL;
    bufsize_t live_size = 0, live_alloc = 0;
    bufsize_t i;

    /* THE LIVE SET, over the whole tree and not one block.
     *
     * The per-block filter in `markdown_core_parser_refine_inline_regions` runs
     * while the block's inlines are being parsed, and TWO things rewrite the
     * tree after that: consolidation, and every extension `postprocess_func` --
     * the autolinker splits a text node into three and frees the original, at
     * `finish`, long after the block's regions were settled. A region naming a
     * node that is gone is read by `S_write_concrete`, so it is a
     * use-after-free rather than a wrong label, and only a pointer set built
     * after the LAST rewrite can rule it out. */
    {
        markdown_core_iter *iter = markdown_core_iter_new(parser->root);
        markdown_core_event_type event;
        if (!iter) {
            parser->oom = true;
            return;
        }
        while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
            markdown_core_node *node;
            if (event != MARKDOWN_CORE_EVENT_ENTER) {
                continue;
            }
            node = markdown_core_iter_get_node(iter);
            if (live_size == live_alloc) {
                markdown_core_node **grown;
                live_alloc = live_alloc ? live_alloc * 2 : 256;
                grown = (markdown_core_node **)parser->mem->realloc(live, (size_t)live_alloc * sizeof(*grown));
                if (!grown) {
                    parser->mem->free(live);
                    markdown_core_iter_free(iter);
                    parser->oom = true;
                    return;
                }
                live = grown;
            }
            live[live_size++] = node;
        }
        markdown_core_iter_free(iter);
        qsort(live, (size_t)live_size, sizeof(*live), S_compare_node_pointers);
    }

    for (i = 0; i < parser->regions_size; i++) {
        markdown_core_node *owner = parser->regions[i].owner;
        bufsize_t start, stop;
        bufsize_t lo = 0, hi = live_size;
        int found = 0;
        while (lo < hi) {
            bufsize_t mid = lo + (hi - lo) / 2;
            if (live[mid] < owner) {
                lo = mid + 1;
            } else if (live[mid] > owner) {
                hi = mid;
            } else {
                found = 1;
                break;
            }
        }
        if (!found) {
            /* Gone. Its bytes are the document's, in the role a byte nothing
             * claims always has. */
            parser->regions[i].owner = parser->root;
            parser->regions[i].role = (uint8_t)MARKDOWN_CORE_REGION_DISCARDED;
            continue;
        }
        /* UP THE ANCESTOR CHAIN, not one step. A pointer freed by a
         * postprocess can be REUSED by a node the same postprocess allocates,
         * so membership above proves the owner is a node and not that it is
         * the same node; the scope is what decides. One step up is not enough,
         * because the impostor's parent is somewhere else in the document
         * too -- measured, three rows a thousand bytes past their owner. The
         * document's scope covers every byte, so the walk terminates. */
        if (!MARKDOWN_CORE_NODE_INLINE_P(owner)) {
            /* A BLOCK owner is left exactly as it is. Its out-of-scope rows are
             * the registered families in `specs/concrete/records.json`, and
             * sweeping them here would hide the defects rather than fix them. */
            continue;
        }
        /* UP THE ANCESTOR CHAIN until the scope contains the bytes. A pointer
         * freed by a postprocess can be REUSED by a node the same postprocess
         * allocates, so membership above proves the owner is a node and not
         * that it is the same node; the scope is what decides. One step up is
         * not enough, because the impostor's parent is somewhere else in the
         * document too -- measured, three rows a thousand bytes past their
         * owner. The document's scope covers every byte, so it terminates. */
        while (owner && owner->parent) {
            if (owner->start_line >= 1 && owner->start_line <= parser->line_starts_size && owner->end_line >= 1 &&
                owner->end_line <= parser->line_starts_size) {
                start = parser->line_starts[owner->start_line - 1] + owner->start_column - 1;
                stop = parser->line_starts[owner->end_line - 1] + owner->end_column + 1;
                if (parser->regions[i].start >= start && parser->regions[i].start + parser->regions[i].length <= stop) {
                    break;
                }
            }
            owner = owner->parent;
            parser->regions[i].owner = owner;
            /* A BLOCK taking the bytes takes them as CONTENT, because the
             * block-level attribution is 11a's and 11b may not re-label it. An
             * INLINE node taking them keeps the role they had: the bytes are
             * still whatever they were, one owner further out. */
            if (!MARKDOWN_CORE_NODE_INLINE_P(owner)) {
                parser->regions[i].role = (uint8_t)MARKDOWN_CORE_REGION_CONTENT;
            }
        }
    }
    parser->mem->free(live);
}

/* Fold adjacent regions with one owner and one role into one. */
static void S_merge_adjacent_regions(markdown_core_parser *parser) {
    bufsize_t read, write = 0;
    if (parser->regions_size == 0) {
        return;
    }
    for (read = 1; read < parser->regions_size; read++) {
        if (parser->regions[write].owner == parser->regions[read].owner &&
            parser->regions[write].role == parser->regions[read].role &&
            parser->regions[write].start + parser->regions[write].length == parser->regions[read].start) {
            parser->regions[write].length += parser->regions[read].length;
        } else {
            parser->regions[++write] = parser->regions[read];
        }
    }
    parser->regions_size = write + 1;
}

/* REQUIREMENT 11b. Claim the bytes [`from`, `to`) of the block being
 * inline-parsed for `owner`, in `role`. See the extension-API declaration for
 * the contract and markdown_core_inline_claim for the role rule. */
void markdown_core_parser_claim_inline(markdown_core_parser *parser, markdown_core_node *owner, bufsize_t from,
                                       bufsize_t to, int role) {
    markdown_core_inline_claim *claim;

    if (!parser || !owner || to <= from || from < 0) {
        return;
    }
    if (parser->inline_claims_size == parser->inline_claims_alloc) {
        bufsize_t alloc = parser->inline_claims_alloc ? parser->inline_claims_alloc * 2 : 64;
        markdown_core_inline_claim *grown;
        if (parser->inline_claims_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return;
        }
        grown =
            (markdown_core_inline_claim *)parser->mem->realloc(parser->inline_claims, (size_t)alloc * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->inline_claims = grown;
        parser->inline_claims_alloc = alloc;
    }
    claim = &parser->inline_claims[parser->inline_claims_size++];
    claim->from = from;
    claim->to = to;
    claim->owner = owner;
    /* A CLAIM FOR THE BLOCK ITSELF IS ALWAYS `CONTENT`, whatever the caller
     * asked for. 11b REFINES a block's content into inline owners; it does not
     * re-label it. A byte the inline phase read and kept nowhere -- the spaces
     * a text run's rtrim dropped, the next line's indent a soft break skipped,
     * the delimiter run an extension is about to consume -- is still a byte the
     * block's content buffer holds, and calling it DISCARDED or MARKER makes
     * the BLOCK-level attribution depend on the inline phase. That breaks L4's
     * first half: the same bytes read as CONTENT in a prefix whose block had
     * not yet grown the construct. Measured, eleven rows. */
    claim->role = MARKDOWN_CORE_NODE_INLINE_P(owner) ? (uint8_t)role : (uint8_t)MARKDOWN_CORE_REGION_CONTENT;
}

/* Resolve the claims made while `b`'s inlines were parsed, and REFINE `b`'s
 * CONTENT regions into them.
 *
 * A region may be refined -- split into adjacent regions covering the same
 * bytes -- and may never be moved or deleted (see markdown_core_region). This
 * is the only refiner in the engine, and everything it produces covers exactly
 * the bytes `b` already owned in role CONTENT.
 *
 * THREE PASSES AND ONE MOVE, all bounded by the block:
 *   1. paint one claim index per content byte, in claim order, so later claims
 *      win by overwriting;
 *   2. group the paint into runs, cut each run at the block's own line marks --
 *      a content range that crosses a line ending is TWO source ranges, because
 *      the next line's stripped indent lies between them -- and collect the
 *      pieces in source order;
 *   3. rewrite `b`'s CONTENT rows, counting first, growing once and
 *      right-aligning the survivors so the forward fill cannot overtake its
 *      own source. That is the same shape S_partition_definition_regions uses
 *      and for the same reason.
 */
void markdown_core_parser_refine_inline_regions(markdown_core_parser *parser, markdown_core_node *b, bufsize_t base) {
    bufsize_t content_length = b->content.size;
    bufsize_t i, at, produced = 0, delta, read, write;
    bufsize_t pieces_size = 0;
    markdown_core_region *pieces = NULL;
    bufsize_t lo, hi;

    if (parser->inline_claims_size <= base || content_length <= 0 || b->content_mark_count <= 0) {
        parser->inline_claims_size = base;
        return;
    }

    /* 1. Paint. */
    if (parser->inline_paint_alloc < content_length) {
        bufsize_t alloc = parser->inline_paint_alloc ? parser->inline_paint_alloc : 256;
        int32_t *grown;
        while (alloc < content_length) {
            if (alloc > (bufsize_t)(INT32_MAX / 2)) {
                parser->oom = true;
                parser->inline_claims_size = base;
                return;
            }
            alloc *= 2;
        }
        grown = (int32_t *)parser->mem->realloc(parser->inline_paint, (size_t)alloc * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            parser->inline_claims_size = base;
            return;
        }
        parser->inline_paint = grown;
        parser->inline_paint_alloc = alloc;
    }
    for (i = 0; i < content_length; i++) {
        parser->inline_paint[i] = -1;
    }
    for (i = base; i < parser->inline_claims_size; i++) {
        bufsize_t stop = parser->inline_claims[i].to;
        if (stop > content_length) {
            stop = content_length;
        }
        for (at = parser->inline_claims[i].from; at < stop; at++) {
            parser->inline_paint[at] = (int32_t)i;
        }
    }

    /* 2. Runs, cut at the line marks, in source order. */
    {
        bufsize_t capacity = 0;
        int mark = b->content_mark;
        int last_mark = b->content_mark + b->content_mark_count - 1;
        at = 0;
        while (at < content_length) {
            int32_t which = parser->inline_paint[at];
            bufsize_t run = at + 1;
            bufsize_t source_start;
            int line, column;
            while (run < content_length && parser->inline_paint[run] == which) {
                run++;
            }
            /* Cut at the next mark: a mark begins a new source line. */
            while (mark < last_mark && parser->line_marks[mark + 1].content_offset <= at) {
                mark++;
            }
            if (mark < last_mark && parser->line_marks[mark + 1].content_offset < run) {
                run = parser->line_marks[mark + 1].content_offset;
            }
            if (!markdown_core_parser_content_place(parser, b, at, &line, &column) || line < 1 ||
                line > parser->line_starts_size) {
                at = run;
                continue;
            }
            source_start = parser->line_starts[line - 1] + column - 1;
            if (pieces_size == capacity) {
                markdown_core_region *grown;
                capacity = capacity ? capacity * 2 : 64;
                grown = (markdown_core_region *)parser->mem->realloc(pieces, (size_t)capacity * sizeof(*grown));
                if (!grown) {
                    parser->mem->free(pieces);
                    parser->oom = true;
                    parser->inline_claims_size = base;
                    return;
                }
                pieces = grown;
            }
            pieces[pieces_size].start = source_start;
            pieces[pieces_size].length = run - at;
            pieces[pieces_size].owner = which < 0 ? b : parser->inline_claims[which].owner;
            pieces[pieces_size].role =
                which < 0 ? (uint8_t)MARKDOWN_CORE_REGION_CONTENT : parser->inline_claims[which].role;
            pieces_size++;
            at = run;
        }
    }
    parser->inline_claims_size = base;
    if (pieces_size == 0) {
        parser->mem->free(pieces);
        return;
    }

    /* TWO SOUNDNESS FILTERS, and both are invariants of this function rather
     * than properties a handler has to get right.
     *
     * A claim's owner may have been FREED between the claim and here -- an
     * extension that matches a delimiter pair builds its own node and frees the
     * text nodes the run was, and consolidation frees every text node but the
     * first of a run. A region naming a freed node is read by
     * `S_write_concrete`, so it is a use-after-free and not a wrong label.
     *
     * And a claim's range may lie OUTSIDE its owner's scope -- an extension
     * that splits a text node shortens the node after the claim was made. That
     * is L2's first clause, and making it hold here means the oracle checks a
     * property the code cannot violate rather than one it happens not to.
     *
     * Either way the bytes fall back to the BLOCK in role CONTENT, which is the
     * other half of 11b's law and is always true of them. */
    {
        markdown_core_node **live = NULL;
        bufsize_t live_size = 0, live_alloc = 0;
        markdown_core_node *stack[MARKDOWN_CORE_MAX_INLINE_DEPTH];
        int depth = 0;
        stack[depth++] = b;
        while (depth > 0) {
            markdown_core_node *node = stack[--depth];
            markdown_core_node *child;
            if (live_size == live_alloc) {
                markdown_core_node **grown;
                live_alloc = live_alloc ? live_alloc * 2 : 64;
                grown = (markdown_core_node **)parser->mem->realloc(live, (size_t)live_alloc * sizeof(*grown));
                if (!grown) {
                    parser->mem->free(live);
                    parser->mem->free(pieces);
                    parser->oom = true;
                    return;
                }
                live = grown;
            }
            live[live_size++] = node;
            for (child = node->first_child; child; child = child->next) {
                if (depth < MARKDOWN_CORE_MAX_INLINE_DEPTH) {
                    stack[depth++] = child;
                }
            }
        }
        qsort(live, (size_t)live_size, sizeof(*live), S_compare_node_pointers);
        for (i = 0; i < pieces_size; i++) {
            markdown_core_node *owner = pieces[i].owner;
            bufsize_t lo2 = 0, hi2 = live_size;
            bufsize_t stop_at;
            int found = 0;
            while (lo2 < hi2) {
                bufsize_t mid = lo2 + (hi2 - lo2) / 2;
                if (live[mid] < owner) {
                    lo2 = mid + 1;
                } else if (live[mid] > owner) {
                    hi2 = mid;
                } else {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                pieces[i].owner = b;
                pieces[i].role = (uint8_t)MARKDOWN_CORE_REGION_CONTENT;
                continue;
            }
            if (owner == b || owner->start_line < 1 || owner->start_line > parser->line_starts_size ||
                owner->end_line < 1 || owner->end_line > parser->line_starts_size) {
                continue;
            }
            stop_at = parser->line_starts[owner->end_line - 1] + owner->end_column;
            if (pieces[i].start < parser->line_starts[owner->start_line - 1] + owner->start_column - 1 ||
                pieces[i].start + pieces[i].length > stop_at + 1) {
                pieces[i].owner = b;
                pieces[i].role = (uint8_t)MARKDOWN_CORE_REGION_CONTENT;
            }
        }
        parser->mem->free(live);
    }
    /* The filters above can leave adjacent pieces with one owner and one role,
     * and a region set is a partition of bytes rather than a record of how many
     * times the parse looked. */
    {
        bufsize_t write = 0;
        for (i = 1; i < pieces_size; i++) {
            if (pieces[write].owner == pieces[i].owner && pieces[write].role == pieces[i].role &&
                pieces[write].start + pieces[write].length == pieces[i].start) {
                pieces[write].length += pieces[i].length;
            } else {
                pieces[++write] = pieces[i];
            }
        }
        pieces_size = write + 1;
    }

    /* 3. Rewrite `b`'s CONTENT rows. The pieces are in source order and cover
     * exactly the bytes those rows cover, so the rewrite is a merge. */
    lo = 0;
    hi = parser->regions_size;
    while (lo < hi) {
        bufsize_t mid = lo + (hi - lo) / 2;
        if (parser->regions[mid].start + parser->regions[mid].length <= pieces[0].start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    hi = lo;
    while (hi < parser->regions_size &&
           parser->regions[hi].start < pieces[pieces_size - 1].start + pieces[pieces_size - 1].length) {
        hi++;
    }

    {
        bufsize_t piece = 0;
        for (i = lo; i < hi; i++) {
            bufsize_t at = parser->regions[i].start;
            bufsize_t end = at + parser->regions[i].length;
            int refinable = S_region_is_refinable(&parser->regions[i], b);
            while (piece < pieces_size && pieces[piece].start < end) {
                bufsize_t ps = pieces[piece].start > at ? pieces[piece].start : at;
                bufsize_t pe = pieces[piece].start + pieces[piece].length;
                if (pe > end) {
                    pe = end;
                }
                if (refinable && pe > ps) {
                    if (ps > at) {
                        produced++;
                    }
                    produced++;
                    at = pe;
                }
                if (pieces[piece].start + pieces[piece].length <= end) {
                    piece++;
                } else {
                    break;
                }
            }
            if (!refinable || at < end) {
                produced++;
            }
        }
    }
    delta = produced - (hi - lo);
    if (delta > 0) {
        bufsize_t alloc = parser->regions_alloc ? parser->regions_alloc : 256;
        while (parser->regions_size + delta > alloc) {
            if (alloc > (bufsize_t)(INT32_MAX / 2)) {
                parser->mem->free(pieces);
                parser->oom = true;
                return;
            }
            alloc *= 2;
        }
        if (alloc != parser->regions_alloc) {
            markdown_core_region *grown =
                (markdown_core_region *)parser->mem->realloc(parser->regions, (size_t)alloc * sizeof(*grown));
            if (!grown) {
                parser->mem->free(pieces);
                parser->oom = true;
                return;
            }
            parser->regions = grown;
            parser->regions_alloc = alloc;
        }
    }
    memmove(&parser->regions[lo + produced], &parser->regions[hi],
            (size_t)(parser->regions_size - hi) * sizeof(*parser->regions));
    memmove(&parser->regions[lo + delta], &parser->regions[lo], (size_t)(hi - lo) * sizeof(*parser->regions));

    read = lo + delta;
    write = lo;
    {
        bufsize_t piece = 0;
        for (i = 0; i < hi - lo; i++) {
            markdown_core_region source = parser->regions[read++];
            bufsize_t at = source.start;
            bufsize_t end = at + source.length;
            /* A region a NESTED refine already wrote -- a directive's label is
             * inline-parsed from inside its own paragraph's inline parse -- is
             * owned by a descendant of this block and is not this refine's to
             * touch. Pieces are CLIPPED to the region they fall in, so one
             * claim spanning both a refinable region and a nested one gives up
             * the half that is not its business. */
            int refinable = S_region_is_refinable(&source, b);
            while (piece < pieces_size && pieces[piece].start < end) {
                bufsize_t ps = pieces[piece].start > at ? pieces[piece].start : at;
                bufsize_t pe = pieces[piece].start + pieces[piece].length;
                if (pe > end) {
                    pe = end;
                }
                if (refinable && pe > ps) {
                    if (ps > at) {
                        /* A refinable region can reach further than this
                         * block's content does -- a table's CONTENT row covers
                         * the pipes as well as the cell -- so what the pieces
                         * do not cover keeps the owner and role it had. */
                        parser->regions[write] = source;
                        parser->regions[write].start = at;
                        parser->regions[write].length = ps - at;
                        write++;
                    }
                    parser->regions[write] = pieces[piece];
                    parser->regions[write].start = ps;
                    parser->regions[write].length = pe - ps;
                    write++;
                    at = pe;
                }
                if (pieces[piece].start + pieces[piece].length <= end) {
                    piece++;
                } else {
                    break;
                }
            }
            if (!refinable || at < end) {
                parser->regions[write] = source;
                parser->regions[write].start = refinable ? at : source.start;
                parser->regions[write].length = end - (refinable ? at : source.start);
                write++;
            }
        }
    }
    assert(write == lo + produced);
    parser->regions_size += delta;
    parser->mem->free(pieces);
}

/* Note that a line begins at `start` in the normalized source.
 *
 * Returns false only when the index could not grow, in which case the parse is
 * already marked lost: a line index missing a line would answer a source offset
 * with the wrong line, silently. */
static bool S_record_line_start(markdown_core_parser *parser, bufsize_t start) {
    if (parser->line_starts_size == parser->line_starts_alloc) {
        bufsize_t alloc = parser->line_starts_alloc ? parser->line_starts_alloc * 2 : 128;
        bufsize_t *grown;
        if (parser->line_starts_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return false;
        }
        grown = (bufsize_t *)parser->mem->realloc(parser->line_starts, (size_t)alloc * sizeof(bufsize_t));
        if (!grown) {
            parser->oom = true;
            return false;
        }
        parser->line_starts = grown;
        parser->line_starts_alloc = alloc;
    }
    parser->line_starts[parser->line_starts_size++] = start;
    return true;
}

/* Record where the bytes about to be appended to `node`'s content came from.
 *
 * `column` is a BYTE column counted from 1, which is what every position in
 * the tree is counted in; `parser->column` is not one, because it counts a tab
 * as the several columns it expands to. */
static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column) {
    markdown_core_line_mark *mark;

    if (parser->line_marks_size == parser->line_marks_alloc) {
        /* One mark per line, so the doubling never has a realistic ceiling to
         * reach; the guard is here because it is cheaper than reasoning about
         * whether it can. */
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return;
        }
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }

    if (node->content_mark_count == 0) {
        node->content_mark = (int)parser->line_marks_size;
    } else {
        /* A block's marks are contiguous because only the deepest open block
         * takes lines and opening another one closes it. If that ever stops
         * being true the run below stops describing this block, silently. */
        assert(node->content_mark + node->content_mark_count == (int)parser->line_marks_size);
    }

    mark = &parser->line_marks[parser->line_marks_size++];
    mark->content_offset = node->content.size;
    mark->line = parser->line_number;
    mark->column = (int)column;
    node->content_mark_count++;
}

static void add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    /* Indentation stripped ahead of the content belongs to the CONTAINER that
     * stripped it, not to the block being written into -- the same rule the
     * block openers follow, and for the same reason: a block begins at its own
     * first non-space byte, so a region of its own that started earlier would
     * start before its own scope. The bytes that ARE copied are its content,
     * and the tab below is one of them, because its expansion is what lands in
     * the buffer. */
    S_claim_region(parser, node->parent ? node->parent : node, parser->offset, MARKDOWN_CORE_REGION_DISCARDED);
    if (parser->partially_consumed_tab) {
        /* The spaces below stand for the tail of the tab at parser->offset and
         * have no source bytes of their own, so they are marked against the
         * tab itself and the copied bytes get a mark of their own. */
        S_record_content_mark(parser, node, parser->offset + 1);
        parser->offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    S_record_content_mark(parser, node, parser->offset + 1);
    markdown_core_strbuf_put(&node->content, ch->data + parser->offset, ch->len - parser->offset);
    S_claim_region(parser, node, ch->len, MARKDOWN_CORE_REGION_CONTENT);
    if (node->content.oom) {
        parser->oom = true;
    }
}

/* Declare that `node`'s content, which was SET rather than fed, begins at
 * (line, column) in the source -- and that it runs on from there without a
 * break.
 *
 * A block whose content the parser copied in line by line gets its marks from
 * `add_line`. A block whose content an extension HANDED it -- a table cell cut
 * out of a row, a directive's label -- has none, and every position inside it
 * then falls back to arithmetic on the block's own start column. One mark is
 * the whole answer for content that is one line long, which is what all of
 * those are.
 *
 * Returns false only when the mark could not be recorded, and the parse is
 * marked lost when that happens: a block with a WRONG map is worse than one
 * with none, because the fallback at least knows it is guessing. */
int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line, int column) {
    markdown_core_line_mark *mark;

    if (!parser || !node) {
        return 0;
    }
    if (parser->line_marks_size == parser->line_marks_alloc) {
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return 0;
        }
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return 0;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }
    mark = &parser->line_marks[parser->line_marks_size];
    mark->content_offset = 0;
    mark->line = line;
    mark->column = column;
    node->content_mark = (int)parser->line_marks_size++;
    node->content_mark_count = 1;
    return 1;
}

/* Copy the marks covering [from, from + length) of `owner`'s content onto
 * `node`, rebased so the first covers `node`'s own offset zero.
 *
 * For content that is a SLICE of another block's content and more than one line
 * long -- the paragraph a table was split out of -- where one mark would put
 * every line of it on the first line's row. The marks are COPIED and not
 * shared: two nodes naming one run is an alias, and an alias between two trees
 * is the shape §1 records six times.
 */
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length) {
    int first;
    int last;
    int i;
    int count;

    if (!parser || !owner || !node || owner->content_mark_count <= 0) {
        return 0;
    }
    first = owner->content_mark;
    last = owner->content_mark + owner->content_mark_count - 1;
    while (first < last && parser->line_marks[first + 1].content_offset <= from) {
        first++;
    }
    while (last > first && parser->line_marks[last].content_offset >= from + length) {
        last--;
    }
    count = last - first + 1;

    while (parser->line_marks_size + count > parser->line_marks_alloc) {
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return 0;
        }
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return 0;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }

    node->content_mark = (int)parser->line_marks_size;
    node->content_mark_count = count;
    for (i = 0; i < count; i++) {
        markdown_core_line_mark copy = parser->line_marks[first + i];
        if (copy.content_offset <= from) {
            copy.column += (int)(from - copy.content_offset);
            copy.content_offset = 0;
        } else {
            copy.content_offset -= from;
        }
        parser->line_marks[parser->line_marks_size++] = copy;
    }
    return 1;
}

/* Requirement 10: for any block with a content buffer and any byte offset
 * within it, name the source line and column of that byte.
 *
 * The answer is a projection of the block's mark run, not a counter anyone
 * maintains: find the slice the offset falls in and add the distance from its
 * start. Binary search, so a caller that asks once per inline node pays
 * log(lines in the block) rather than re-walking it. */
int markdown_core_parser_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t content_offset,
                                       int *line, int *column) {
    const markdown_core_line_mark *mark;
    int lo, hi;

    if (!parser || !node || node->content_mark_count <= 0 || content_offset < 0) {
        return 0;
    }

    lo = node->content_mark;
    hi = lo + node->content_mark_count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (parser->line_marks[mid].content_offset <= content_offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    mark = &parser->line_marks[lo];
    *line = mark->line;
    *column = mark->column + (int)(content_offset - mark->content_offset);
    return 1;
}

/* Drop `dropped` bytes off the FRONT of `node`'s content, leaving `remaining`
 * bytes, and keep the map describing what is left. The marks stay where they are in the vector: the
 * run's head moves past the slices that went away, and the slice the cut
 * landed inside keeps its line with its column advanced to the cut. */
static void S_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                   bufsize_t remaining) {
    int i;
    int first = node->content_mark;
    int last = node->content_mark + node->content_mark_count - 1;

    if (node->content_mark_count <= 0 || dropped <= 0) {
        return;
    }

    if (remaining <= 0) {
        /* The cut took everything recorded so far, so no slice survives it and
         * the run is EMPTY rather than one mark advanced past the end of its
         * own line. Keeping the last mark here read as "the block starts on
         * the last line it consumed", which is how a paragraph of nothing but
         * reference definitions came to report a start_line four lines below
         * where it was written -- and, through that, how the region set came
         * to name a node it had already freed. */
        node->content_mark_count = 0;
        return;
    }

    while (first < last && parser->line_marks[first + 1].content_offset <= dropped) {
        first++;
    }

    parser->line_marks[first].column += (int)(dropped - parser->line_marks[first].content_offset);
    parser->line_marks[first].content_offset = 0;
    for (i = first + 1; i <= last; i++) {
        parser->line_marks[i].content_offset -= dropped;
    }
    node->content_mark = first;
    node->content_mark_count = last - first + 1;
}

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    bufsize_t i;
    unsigned char c;

    for (i = ln->size - 1; i >= 0; --i) {
        c = ln->ptr[i];

        if (c != ' ' && c != '\t' && !S_is_line_end_char(c)) {
            break;
        }
    }

    if (i < 0) {
        markdown_core_strbuf_clear(ln);
        return;
    }

    for (; i < ln->size; ++i) {
        c = ln->ptr[i];

        if (!S_is_line_end_char(c)) {
            continue;
        }

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

/* The source offset a block's content offset was written at, or -1 when the
 * content-to-source map cannot answer -- which happens only where a mark was
 * lost to allocation failure, and that has already failed the parse. */
static bufsize_t S_content_source_offset(markdown_core_parser *parser, markdown_core_node *b,
                                         bufsize_t content_offset) {
    int line, column;
    if (!markdown_core_parser_content_place(parser, b, content_offset, &line, &column)) {
        return -1;
    }
    if (line < 1 || line > parser->line_starts_size) {
        return -1;
    }
    return parser->line_starts[line - 1] + column - 1;
}

/* THE DEFINITION IS A NODE (the rule above `markdown_core_definition`).
 *
 * A link reference definition read off the front of `b`'s content becomes a
 * `ReferenceDefinition` spliced in ahead of `b`, at the byte where its opening
 * bracket was written, owning every byte it read. Upstream drops those bytes
 * into a parser-private map and frees the paragraph that held them; keeping
 * them is what makes the block partition total for a definition-bearing
 * document, and it is why nothing here has to remember that a node was
 * destroyed.
 *
 * `from` and `upto` are offsets into `b`'s content, read BEFORE the harvest
 * drops it, so the content-to-source map still describes them.
 *
 * Q7 and Q26: the destination is REQUIRED. An allocation that loses it fails
 * the parse rather than producing a definition that lies about where it points.
 */
static markdown_core_node *S_new_reference_definition(markdown_core_parser *parser, markdown_core_node *b,
                                                      bufsize_t from, bufsize_t upto,
                                                      const markdown_core_reference_parts *parts) {
    markdown_core_node *node;
    markdown_core_definition *definition;
    markdown_core_chunk url = parts->url;
    markdown_core_chunk title = parts->title;
    int start_line, start_column, end_line, end_column;
    bufsize_t last = upto;
    int lost = 0;

    /* The scope ends at the last byte the definition read that is not a line
     * ending: a definition consumes the line ending that terminates it, and a
     * block's end names its last byte the way every other block's does. */
    while (last > from && S_is_line_end_char(b->content.ptr[last - 1])) {
        last--;
    }
    /* Both refusals below FAIL THE PARSE rather than dropping the definition
     * quietly. The harvest consumes these bytes either way, so a definition
     * that could not be placed is a document missing source the author wrote
     * while the reference map still resolves the label -- which is D30's shape
     * exactly: a wrong document with the failure bit clear. Neither is
     * reachable except through a lost content mark, and that already sets the
     * bit; saying so here is what keeps it true when the map changes. */
    if (last == from) {
        parser->oom = true;
        return NULL;
    }
    if (!markdown_core_parser_content_place(parser, b, from, &start_line, &start_column) ||
        !markdown_core_parser_content_place(parser, b, last - 1, &end_line, &end_column)) {
        parser->oom = true;
        return NULL;
    }

    node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, parser->mem);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    definition = (markdown_core_definition *)parser->mem->calloc(1, sizeof(*definition));
    if (!definition) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    node->as.definition = definition;
    node->start_line = start_line;
    node->start_column = start_column;
    node->end_line = end_line;
    node->end_column = end_column;

    if (!markdown_core_association_init(parser->mem, &definition->association, &parts->label, 0)) {
        /* The label would keep borrowing the content buffer the harvest drops. */
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    definition->url = markdown_core_clean_url(parser->mem, &url, &lost);
    definition->title = markdown_core_clean_title(parser->mem, &title, &lost);
    if (lost) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }

    if (!markdown_core_node_insert_before(b, node)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    return node;
}

/* The owner of source byte `at`, and where that owner's run ends.
 *
 * Over [`from`, `upto`) the definitions tile the bytes in order, each from its
 * own opening bracket to the end of the last line it read. What is left
 * between one definition's last line ending and the next one's bracket is that
 * line's indentation, and it goes to `parent` -- which is where the FIRST
 * line's indentation already went, before `b` existed to claim it. */
static markdown_core_node *S_definition_run(markdown_core_parser *parser, markdown_core_node **cursor, int *remaining,
                                            markdown_core_node *parent, bufsize_t at, bufsize_t upto,
                                            bufsize_t *run_end) {
    while (*remaining > 0) {
        markdown_core_node *definition = *cursor;
        bufsize_t start = parser->line_starts[definition->start_line - 1] + definition->start_column - 1;
        bufsize_t stop = *remaining > 1 ? parser->line_starts[definition->next->start_line - 1] : upto;
        if (at < start) {
            *run_end = start;
            return parent;
        }
        if (at < stop) {
            *run_end = stop;
            return definition;
        }
        *cursor = definition->next;
        (*remaining)--;
    }
    *run_end = upto;
    return parent;
}

/* Re-attribute the source bytes a run of link reference definitions read.
 *
 * ROLES ARE PRESERVED, and that is not a stylistic choice -- L4 forces it. A
 * prefix of the document that stops before the destination reads `[foo]:` as
 * ordinary paragraph CONTENT; the whole document reads the same bytes as a
 * definition. If becoming a definition changed the role, every such prefix
 * would attribute those bytes differently from the whole document, which is
 * exactly what the completeness law forbids. What changes is the OWNER.
 *
 * ONE PASS AND ONE MOVE. A run of unindented definitions is a SINGLE region --
 * `S_claim_region` extends rather than appends when owner, role and cursor all
 * line up -- so that region has to be cut into as many pieces as there are
 * definitions. Cutting it once per definition would move the tail of the
 * region array once per definition, which is quadratic in a paragraph with
 * many definitions and many regions after them. So the pieces are counted, the
 * array is grown once, the surviving rows are right-aligned inside the window
 * they will occupy, and the pieces are written forward over them. The write
 * pointer can never overtake the read pointer because every source row
 * produces at least one piece. */
static void S_partition_definition_regions(markdown_core_parser *parser, markdown_core_node *b,
                                           markdown_core_node *first, int count, bufsize_t from, bufsize_t upto) {
    markdown_core_node *parent = b->parent ? b->parent : parser->root;
    markdown_core_node *cursor;
    bufsize_t lo = 0, hi, i, produced = 0, delta, read, write;
    int remaining;

    if (count <= 0 || from < 0 || upto <= from) {
        return;
    }

    /* The rows the run touches: [lo, hi), with the last free to run past
     * `upto` -- the paragraph's surviving content shares a row with its
     * definitions whenever nothing broke the run. Found by bisection, because
     * scanning from row zero would cost the whole document on every paragraph
     * that opens with a definition. */
    hi = parser->regions_size;
    while (lo < hi) {
        bufsize_t mid = lo + (hi - lo) / 2;
        if (parser->regions[mid].start + parser->regions[mid].length <= from) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    hi = lo;
    while (hi < parser->regions_size && parser->regions[hi].start < upto) {
        hi++;
    }
    if (lo == hi) {
        return;
    }

    cursor = first;
    remaining = count;
    for (i = lo; i < hi; i++) {
        bufsize_t at = parser->regions[i].start;
        bufsize_t end = at + parser->regions[i].length;
        if (parser->regions[i].owner != b) {
            produced++;
            continue;
        }
        while (at < end) {
            bufsize_t run_end;
            if (at >= upto) {
                produced++;
                break;
            }
            S_definition_run(parser, &cursor, &remaining, parent, at, upto, &run_end);
            produced++;
            at = run_end < end ? run_end : end;
        }
    }

    delta = produced - (hi - lo);
    if (delta > 0) {
        bufsize_t alloc = parser->regions_alloc ? parser->regions_alloc : 256;
        while (parser->regions_size + delta > alloc) {
            if (alloc > (bufsize_t)(INT32_MAX / 2)) {
                parser->oom = true;
                return;
            }
            alloc *= 2;
        }
        if (alloc != parser->regions_alloc) {
            markdown_core_region *grown =
                (markdown_core_region *)parser->mem->realloc(parser->regions, (size_t)alloc * sizeof(*grown));
            if (!grown) {
                parser->oom = true;
                return;
            }
            parser->regions = grown;
            parser->regions_alloc = alloc;
        }
    }

    memmove(&parser->regions[lo + produced], &parser->regions[hi],
            (size_t)(parser->regions_size - hi) * sizeof(*parser->regions));
    memmove(&parser->regions[lo + delta], &parser->regions[lo], (size_t)(hi - lo) * sizeof(*parser->regions));

    cursor = first;
    remaining = count;
    read = lo + delta;
    write = lo;
    for (i = 0; i < hi - lo; i++) {
        markdown_core_region source = parser->regions[read++];
        bufsize_t at = source.start;
        bufsize_t end = at + source.length;
        if (source.owner != b) {
            parser->regions[write++] = source;
            continue;
        }
        while (at < end) {
            bufsize_t run_end;
            markdown_core_node *owner;
            if (at >= upto) {
                owner = b;
                run_end = end;
            } else {
                owner = S_definition_run(parser, &cursor, &remaining, parent, at, upto, &run_end);
            }
            if (run_end > end) {
                run_end = end;
            }
            parser->regions[write] = source;
            parser->regions[write].start = at;
            parser->regions[write].length = run_end - at;
            parser->regions[write].owner = owner;
            write++;
            at = run_end;
        }
    }
    assert(write == lo + produced);
    parser->regions_size += delta;
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_reference_parts parts;
    markdown_core_node *first = NULL;
    bufsize_t consumed = 0;
    bufsize_t span_from = -1;
    int count = 0;
    while (chunk.len && chunk.data[0] == '[' &&
           (pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &parts))) {
        markdown_core_node *definition = S_new_reference_definition(parser, b, consumed, consumed + pos, &parts);
        if (definition) {
            if (!first) {
                first = definition;
                span_from = S_content_source_offset(parser, b, consumed);
            }
            count++;
        }
        consumed += pos;
        chunk.data += pos;
        chunk.len -= pos;
    }
    if (count > 0 && span_from >= 0) {
        bufsize_t span_upto = S_content_source_offset(parser, b, consumed - 1);
        if (span_upto >= 0) {
            S_partition_definition_regions(parser, b, first, count, span_from, span_upto + 1);
        }
    }
    // The definitions are dropped off the FRONT of the block's content, so what
    // is left starts further down the source than the block was told it did.
    // Without this a paragraph whose leading definitions were consumed keeps the
    // DEFINITION's position, and so does every inline in it, because
    // markdown_core_parse_inlines seeds the subject from b->start_line and
    // b->start_column.
    //
    // D18 corrected the LINE here by counting the line endings in the prefix
    // that goes away, and left the column alone with the note that it was
    // right wherever the remaining first line has the same stripped prefix as
    // the definition's line. Requirement 10 removes both the count and the
    // caveat: the map says where the surviving first byte was written, so the
    // column is answered rather than assumed, and the marks are rebased so the
    // inline phase reads the same map against the shortened buffer.
    bufsize_t dropped = node_content->size - chunk.len;
    int line, column;
    S_rebase_content_marks(parser, b, dropped, chunk.len);
    markdown_core_strbuf_drop(node_content, dropped);
    /* The block now begins where its FIRST SURVIVING line was written, and
     * that is asked of the map rather than derived: this function can be
     * reached twice on one paragraph -- once at the setext-underline check and
     * again at finalize -- and the first call can consume everything recorded
     * so far, leaving the line that carries what is left still unread. Taking
     * the answer from the surviving run rather than from the size of the cut
     * is what makes both arrivals give the same result. On a block with no
     * definitions in front of it this is what the block already said. */
    if (markdown_core_parser_content_place(parser, b, 0, &line, &column)) {
        b->start_line = line;
        b->start_column = column;
    }
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
               (S_type(b) == MARKDOWN_CORE_NODE_HEADING && b->as.heading.setext) ||
               /* D35: a block finalized on the line it OPENED did not end on
                * the previous one. `line_number - 1` below assumes the block
                * was closed by a later line, which is true of every block that
                * needs a following line to end it -- and false of an HTML block
                * of type 2 to 5, whose terminator can be on its own first line.
                * Measured: `<!-- c -->` alone on line 3 gave
                * `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at
                * 3:10, and `last_line_length` there is the length of the BLANK
                * line before it. Four of the eleven negative rows in
                * `specs/scope-sanity/ledger.json` were this. */
               parser->line_number == b->start_line) {
        S_set_end_to_current_line(parser, b);
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    markdown_core_strbuf *node_content = &b->content;

    switch (S_type(b)) {
    case MARKDOWN_CORE_NODE_PARAGRAPH: {
        /* Read BEFORE the harvest: it moves `start_line` forward to the first
         * line that survives, and the regions to hand on are the ones from
         * where the block was WRITTEN. */
        int written_at = b->start_line;
        has_content = resolve_reference_link_definitions(parser, b);
        if (!has_content) {
            // remove blank node (former reference def)
            S_disown_regions(parser, b, written_at);
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
                if (S_is_line_end_char(node_content->ptr[pos])) {
                    break;
                }
            }
            assert(pos < node_content->size);

            markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT(parser->mem);
            houdini_unescape_html_f(&tmp, node_content->ptr, pos);
            markdown_core_strbuf_trim(&tmp);
            markdown_core_strbuf_unescape(&tmp);
            b->as.code.info = markdown_core_chunk_buf_detach(&tmp);
            if (!b->as.code.info.data) {
                parser->oom = true;
            }

            if (node_content->ptr[pos] == '\r') {
                pos += 1;
            }
            if (node_content->ptr[pos] == '\n') {
                pos += 1;
            }
            markdown_core_strbuf_drop(node_content, pos);
        }
        b->as.code.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.code.literal.data) {
            parser->oom = true;
        }
        break;

    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        b->as.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.literal.data) {
            parser->oom = true;
        }
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
        if (child) {
            markdown_core_node_free(child);
        }
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

/* Two of the three byte sets are folded into parser tables here; the third,
 * `dispatch`, is asked directly because it also answers ownership questions
 * that a merged table cannot. The two folds are now independent, which is the
 * whole point: before this, one list fed both tables and whether it fed the
 * second was a single `emphasis` bool covering every byte the extension named.
 * That is D1. */
void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_syntax_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)tmp_ext->data;
        const unsigned char *c;

        for (c = (const unsigned char *)ext->terminates_text; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_text_terminator(parser, *c);
            } else {
                markdown_core_inlines_remove_text_terminator(parser, *c);
            }
        }
        for (c = (const unsigned char *)ext->flanking_transparent; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_flanking_transparent(parser, *c);
            } else {
                markdown_core_inlines_remove_flanking_transparent(parser, *c);
            }
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

    process_inlines(parser, parser->refmap, parser->options);

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

/* One reservation for the whole of this chunk's contribution to the held
 * partial line, and then a test.
 *
 * `parser->linebuf.oom` was written at six sites and read at NONE (D27). A
 * refused growth made `markdown_core_strbuf_put` a no-op, and the accumulated
 * PREFIX was then handed to `S_process_line` as though it were a whole line and
 * committed -- with `parser->oom` clear, so `finish` returned a document.
 * Measured on a 279-byte document fed in 32-byte chunks: refusing allocation 6
 * of 25 leaves 55 of 275 text bytes and reports success.
 *
 * Reserving first is what makes the refusal atomic: the NUL path writes twice,
 * and a failure between the two writes leaves a line that is neither the old
 * one nor the new one. The arithmetic is done in 64 bits because `bufsize_t` is
 * int32_t and `size + add` is exactly the overflow A4 closed one level down. */
static bool S_linebuf_reserve(markdown_core_parser *parser, int64_t add) {
    int64_t target = (int64_t)parser->linebuf.size + add;

    if (add < 0 || target > (int64_t)(INT32_MAX / 2)) {
        parser->linebuf.oom = 1;
    } else if (add > 0) {
        markdown_core_strbuf_grow(&parser->linebuf, (bufsize_t)target);
    }
    if (parser->linebuf.oom) {
        parser->oom = true;
        return false;
    }
    return true;
}

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof) {
    const unsigned char *end = buffer + len;
    static const uint8_t repl[] = {239, 191, 189};

    if (len > UINT_MAX - parser->total_size) {
        parser->total_size = UINT_MAX;
    } else {
        parser->total_size += len;
    }

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
                if (!S_linebuf_reserve(parser, chunk_len)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
                markdown_core_strbuf_clear(&parser->linebuf);
            } else {
                S_process_line(parser, buffer, chunk_len);
            }
        } else {
            if (eol < end && *eol == '\0') {
                // omit NULL byte, add replacement character
                if (!S_linebuf_reserve(parser, (int64_t)chunk_len + 3)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                markdown_core_strbuf_put(&parser->linebuf, repl, 3);
            } else {
                if (!S_linebuf_reserve(parser, chunk_len)) {
                    return;
                }
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
                    if (buffer == end) {
                        parser->last_buffer_ended_with_cr = true;
                    }
                }
                if (buffer < end && *buffer == '\n') {
                    buffer++;
                }
            }
        }
    }
}

static void chop_trailing_hashtags(markdown_core_chunk *ch) {
    bufsize_t n, orig_n;

    markdown_core_chunk_rtrim(ch);
    orig_n = n = ch->len - 1;

    // if string ends in space followed by #s, remove these:
    while (n >= 0 && peek_at(ch, n) == '#') {
        n--;
    }

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
                                  markdown_core_chunk *input, bool *should_continue) {
    int matched;

    if (!container->extension->last_block_matches) {
        return false;
    }

    matched =
        container->extension->last_block_matches(container->extension, parser, input->data, input->len, container);
    if (matched != MARKDOWN_CORE_BLOCK_CLOSED) {
        return matched != 0;
    }

    /* The container's own closing line. Everything still open inside it ended
     * on the line before, and the container ends here.
     *
     * `parser->current` is the deepest open block and `container` is on the
     * path from the root to it, so walking up through `finalize` reaches it.
     * `finalize` frees a node only in its PARAGRAPH case and returns the
     * parent either way, so the loop is safe across a paragraph that was
     * nothing but reference definitions. */
    *should_continue = false;
    while (parser->current != container) {
        parser->current = finalize(parser, parser->current);
        assert(parser->current != NULL);
    }
    /* `container` carries an extension-minted type, never PARAGRAPH, so it
     * survives its own finalize and can still be positioned. That is the one
     * lifetime invariant this path rests on. */
    assert(S_type(container) != MARKDOWN_CORE_NODE_PARAGRAPH);
    parser->current = finalize(parser, container);
    S_set_end_to_current_line(parser, container);
    return false;
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
            if (!parse_extension_block(parser, container, input, &should_continue)) {
                goto done;
            }
            S_claim_region(parser, container, parser->offset, MARKDOWN_CORE_REGION_MARKER);
            continue;
        }

        switch (cont_type) {
        case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
            if (!parse_block_quote_prefix(parser, input)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_LIST_ITEM:
            if (!parse_node_item_prefix(parser, input, container)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_CODE_BLOCK:
            if (!parse_code_block_prefix(parser, input, container, &should_continue)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_HEADING:
            // a heading can never contain more than one line
            goto done;
        case MARKDOWN_CORE_NODE_HTML_BLOCK:
            if (!parse_html_block_prefix(parser, container)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_PARAGRAPH:
            if (parser->blank) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
            if (!parse_footnote_definition_block_prefix(parser, input, container)) {
                goto done;
            }
            break;
        default:
            break;
        }

        /* Whatever this container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. One claim per container, walking down the spine. */
        S_claim_region(parser, container, parser->offset, MARKDOWN_CORE_REGION_MARKER);
    }

    *all_matched = true;

done:
    /* A container whose prefix consumed bytes and then declined still read
     * them; they are its marker up to the point it gave up. */
    S_claim_region(parser, container, parser->offset, MARKDOWN_CORE_REGION_MARKER);
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
        /* Indentation ahead of whatever opens here is the CONTAINER's, not the
         * new block's: a block begins at its own first non-space byte, so
         * giving the spaces to the block being opened would make its first
         * region start before its own scope. Measured before it was fixed --
         * 52 rows of an indented code block's four spaces alone. */
        S_claim_region(parser, *container, parser->first_nonspace, MARKDOWN_CORE_REGION_DISCARDED);
        indented = parser->indent >= CODE_INDENT;

        if (!indented && peek_at(input, parser->first_nonspace) == '>') {

            bufsize_t blockquote_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
            // optional following character
            if (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_BLOCK_QUOTE, blockquote_startpos + 1);
            if (!*container) {
                return;
            }

        } else if (!indented && (matched = scan_atx_heading_start(input, parser->first_nonspace))) {
            bufsize_t hashpos;
            int level = 0;
            bufsize_t heading_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
            if (!*container) {
                return;
            }

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
            if (!*container) {
                return;
            }
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
            if (!*container) {
                return;
            }
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
            if (!*container) {
                return;
            }
            S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
        } else if (!indented && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && depth < MAX_LIST_DEPTH &&
                   (matched = scan_footnote_definition(input, parser->first_nonspace))) {
            markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);

            while (c.data[c.len - 1] != ']') {
                --c.len;
            }
            --c.len;

            if (!markdown_core_chunk_to_cstr(parser->mem, &c)) {
                /* The label would keep borrowing the transient line buffer. */
                parser->oom = true;
                return;
            }

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            /* THE ANCHOR RULE (§5.1): a definition is a block node at the byte
             * where its OPENING BRACKET was written. It used to start at the
             * byte after `[^label]:`, which is a column that need not exist --
             * `[^footnote]:` alone on a line is twelve bytes and the definition
             * began at column 13. Every other block in this engine starts at
             * its own first byte and the marker is inside it; a footnote
             * definition was the one that started after its own marker. */
            *container =
                add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION, parser->first_nonspace + 1);
            if (!*container) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            /* The identifier KEEPS the caret the label does not carry
             * (markdown_core_association). */
            if (!markdown_core_association_init(parser->mem, &(*container)->as.association, &c, '^')) {
                parser->oom = true;
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            markdown_core_chunk_free(parser->mem, &c);

            /* The document defines this label from here on.
             *
             * Registered where the label is READ, which is here. Whether it is
             * registered at open or at close is NOT observable and that was
             * measured, not assumed: moving this call into `finalize` leaves
             * every suite and every oracle green. It used to matter, and the
             * reason it stopped is the shape rather than the timing -- the map
             * this replaced held a NODE per entry and used registration order
             * as the tie-break for a repeated label, so on EXIT a definition
             * nested inside another closed first, won the label, and the outer
             * one was freed with everything written in it (D11). A set of
             * labels owns no node and picks no winner, so order decides
             * nothing left to get wrong. */
            markdown_core_footnote_definition_create(parser->footnote_defs, &(*container)->as.literal);

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
            if (!*container) {
                return;
            }
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
                const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)tmp->data;

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

        /* What this opener consumed made the block it just opened, so the
         * block owns it: `> `, `- `, the `#`s of a heading, the opening fence,
         * `[^label]:`. Claimed once per turn of the loop -- once per block
         * opened -- and before `accepts_lines` breaks out. */
        S_claim_region(parser, *container, parser->offset, MARKDOWN_CORE_REGION_MARKER);

        if (accepts_lines(*container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        cont_type = S_type(*container);
        maybe_lazy = false;
    }
    S_claim_region(parser, *container, parser->offset, MARKDOWN_CORE_REGION_MARKER);
}

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    S_find_first_nonspace(parser, input);

    if (parser->blank && container->last_child) {
        S_set_last_line_blank(container->last_child, true);
    }

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
            if (!container) {
                return;
            }
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

    if (parser->oom || parser->root == NULL) {
        return;
    }

    markdown_core_strbuf_clear(&parser->curline);

    if (parser->options & MARKDOWN_CORE_OPT_VALIDATE_UTF8) {
        markdown_core_utf8proc_check(&parser->curline, buffer, bytes);
    } else {
        markdown_core_strbuf_put(&parser->curline, buffer, bytes);
    }

    bytes = parser->curline.size;

    // ensure line ends with a newline:
    if (bytes == 0 || !S_is_line_end_char(parser->curline.ptr[bytes - 1])) {
        markdown_core_strbuf_putc(&parser->curline, '\n');
    }

    if (parser->curline.oom) {
        parser->oom = true;
        return;
    }

    /* The line joins the normalized source HERE, before anything reads it, so
     * the source is complete for lines 1..N the moment line N has been fed --
     * which is requirement 11a's L4 and the reason nothing about the record
     * set may be built at close. */
    if (!S_record_line_start(parser, parser->source.size)) {
        return;
    }
    markdown_core_strbuf_put(&parser->source, parser->curline.ptr, parser->curline.size);
    if (parser->source.oom) {
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
    if (parser->line_number == 0 && input.len >= 3 && memcmp(input.data, "\xef\xbb\xbf", 3) == 0) {
        parser->offset += 3;
    }

    parser->line_number++;

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }

    container = last_matched_container;

    current = parser->current;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->oom) {
        goto finished;
    }

    /* parser->current might have changed if feed_reentrant was called */
    if (current == parser->current) {
        add_text_to_container(parser, container, last_matched_container, &input);
    }

finished:
    /* Whatever is left of the line reached no block's content and no block's
     * marker: indentation a container stripped and nobody kept, the trailing
     * hashes `chop_trailing_hashtags` removed, the line ending of a thematic
     * break. It is DISCARDED, and its owner is the block it was read inside.
     * Measured against `curline.size` rather than `input.len`, because a
     * chopped heading shortens the chunk and the bytes it dropped are exactly
     * what this sweep is for.
     *
     * After this the line tiles, which is L1, and no later phase adds to it,
     * which is L4. */
    S_claim_region(parser, parser->current ? parser->current : parser->root, parser->curline.size,
                   MARKDOWN_CORE_REGION_DISCARDED);
    parser->region_cursor = 0;

    parser->last_line_length = input.len;
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\n') {
        parser->last_line_length -= 1;
    }
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\r') {
        parser->last_line_length -= 1;
    }

    markdown_core_strbuf_clear(&parser->curline);
}

markdown_core_node *markdown_core_parser_finish(markdown_core_parser *parser) {
    markdown_core_node *res;
    markdown_core_llist *extensions;

    /* Parser was already finished once */
    if (parser->root == NULL) {
        return NULL;
    }

    /* The held partial line is the last thing the stream said. If its buffer
     * lost bytes, what is here is a PREFIX, and processing it would commit a
     * line the author did not write. */
    if (parser->linebuf.oom) {
        parser->oom = true;
    } else if (parser->linebuf.size) {
        S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
        markdown_core_strbuf_clear(&parser->linebuf);
    }

    finalize_document(parser);

    /* Adjacent regions with one owner and one role are ONE region: the record
     * set is a partition of bytes, not a record of how many times the parse
     * looked. `S_claim_region` keeps that true as regions are laid down, and
     * every REASSIGNMENT since -- a definition harvest, an inline refinement, a
     * consolidated text run taking its neighbours' bytes -- can put two
     * identical rows side by side. This is the one place that can see all of
     * them, because consolidation is the last thing that moves an owner. */
    if (!markdown_core_consolidate_text_nodes_with_parser(parser, parser->root)) {
        parser->oom = true;
    }

    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);

#if MARKDOWN_CORE_DEBUG_NODES
    if (markdown_core_node_check(parser->root, stderr)) {
        abort();
    }
#endif

    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
        if (ext->postprocess_func) {
            markdown_core_node *processed = ext->postprocess_func(ext, parser, parser->root);
            if (processed) {
                parser->root = processed;
            }
        }
    }

    if (parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) {
        if (!S_strip_html_comments(parser->root)) {
            parser->oom = true;
        }
    }

    /* AFTER THE LAST REWRITE, and that is why it is here rather than beside
     * consolidation: an extension's postprocess and the comment strip both free
     * and re-parent inline nodes, and a region set that named them is only
     * sound once nothing else will move. */
    S_reseat_inline_regions(parser);
    S_merge_adjacent_regions(parser);

    /* All allocation-loss routes converge here: block/inline structures set
     * parser->oom directly, definition maps carry their own sticky flag. */
    if (parser->refmap && parser->refmap->oom) {
        parser->oom = true;
    }
    /* The definition set is the second such map and it converges here for the
     * same reason: a normalization it could not allocate answers "this label is
     * not defined", which degrades a footnote call to text and looks exactly
     * like a document that never had one. The allocation-failure sweep caught
     * it -- `quote with footnote[^fn] and ` came back as prose with the parse
     * reporting success. */
    if (parser->footnote_defs && parser->footnote_defs->oom) {
        parser->oom = true;
    }
    if (parser->oom) {
        markdown_core_node_free(parser->root);
        parser->root = NULL;
        markdown_core_parser_reset(parser);
        return NULL;
    }

    if (parser->concrete_out) {
        S_write_concrete(parser, parser->concrete_out);
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
