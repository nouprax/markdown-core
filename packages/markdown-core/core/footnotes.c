#include <stdio.h>
#include <stdlib.h>

#include "markdown-core.h"
#include "parser.h"
#include "footnotes.h"
#include "inlines.h"
#include "chunk.h"
#include "iterator.h"
#include "node.h"

static void footnote_free(markdown_core_map *map, markdown_core_map_entry *_ref) {
    markdown_core_footnote *ref = (markdown_core_footnote *)_ref;
    markdown_core_mem *mem = map->mem;
    if (ref != NULL) {
        mem->free(ref->entry.label);
        if (ref->node) {
            markdown_core_node_free(ref->node);
        }
        mem->free(ref);
    }
}

void markdown_core_footnote_create(markdown_core_map *map, markdown_core_node *node) {
    markdown_core_footnote *ref;
    unsigned char *reflabel;
    int lost = 0;

    if (map == NULL) {
        return;
    }

    reflabel = markdown_core_map_normalize_label(map->mem, &node->as.literal, &lost);

    /* empty footnote name, or composed from only whitespace */
    if (reflabel == NULL) {
        if (lost) {
            map->oom = 1;
        }
        return;
    }

    ref = (markdown_core_footnote *)map->mem->calloc(1, sizeof(*ref));
    if (!ref) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    ref->entry.label = reflabel;
    ref->node = node;

    markdown_core_map_add(map, &ref->entry);
}

markdown_core_map *markdown_core_footnote_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, footnote_free);
}

/* --- Projection ----------------------------------------------------------
 *
 * The projection recomputes the v1 footnote semantics from the finished
 * tree in three phases; each phase is a separate function so the
 * incremental engine can re-drive them per commit later.
 */

/* Phase 1: every footnote definition in document order. Duplicate labels
 * all enter the map; the map's document-order winner is the first
 * definition, exactly the v1 duplicate rule. */
static void S_collect_definitions(markdown_core_parser *parser, markdown_core_map *map) {
    markdown_core_iter *iter = markdown_core_iter_new(parser->root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    if (!iter) {
        parser->oom = true;
        return;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && cur->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            markdown_core_footnote_create(map, cur);
        }
    }

    markdown_core_iter_free(iter);
}

/* Phase 2: references in document order. The first reference to a label
 * assigns the definition's index; every reference renumbers its literal to
 * that index and records its per-definition ordinal. References without a
 * definition degrade to literal text. */
static void S_resolve_references(markdown_core_parser *parser, markdown_core_map *map) {
    markdown_core_iter *iter = markdown_core_iter_new(parser->root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;
    unsigned int ix = 0;

    if (!iter) {
        parser->oom = true;
        return;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_EXIT || cur->type != MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE) {
            continue;
        }
        markdown_core_footnote *footnote = (markdown_core_footnote *)markdown_core_map_lookup(map, &cur->as.literal);
        if (footnote) {
            if (!footnote->ix) {
                footnote->ix = ++ix;
            }

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
                if (!text->as.literal.data) {
                    parser->oom = true;
                }
                markdown_core_node_insert_after(cur, text);
                markdown_core_node_free(cur);
            } else {
                parser->oom = true;
            }
        }
    }

    markdown_core_iter_free(iter);
}

static int sort_footnote_by_ix(const void *_a, const void *_b) {
    markdown_core_footnote *a = *(markdown_core_footnote **)_a;
    markdown_core_footnote *b = *(markdown_core_footnote **)_b;
    return (int)a->ix - (int)b->ix;
}

/* Phase 3: used definitions move to the document tail in first-use order;
 * unused ones leave the tree (their nodes are freed with the map). When the
 * winner array cannot be allocated, emission is skipped and every
 * definition is dropped the same way; map->oom reports the loss. */
static void S_apply_placement(markdown_core_parser *parser, markdown_core_map *map) {
    size_t count = 0;
    markdown_core_map_entry **winners = markdown_core_map_winners(map, &count);
    unsigned int i;

    if (!winners) {
        return;
    }

    qsort(winners, count, sizeof(markdown_core_map_entry *), sort_footnote_by_ix);
    for (i = 0; i < count; ++i) {
        markdown_core_footnote *footnote = (markdown_core_footnote *)winners[i];
        if (!footnote->ix) {
            markdown_core_node_unlink(footnote->node);
            continue;
        }
        markdown_core_node_append_child(parser->root, footnote->node);
        footnote->node = NULL;
    }
    parser->mem->free(winners);
}

void markdown_core_process_footnotes(markdown_core_parser *parser) {
    markdown_core_map *map = markdown_core_footnote_map_new(parser->mem);
    if (!map) {
        parser->oom = true;
        return;
    }

    S_collect_definitions(parser, map);
    if (!parser->oom) {
        S_resolve_references(parser, map);
    }
    /* A prepared map means at least one reference performed a lookup; with
     * no references at all there is nothing to place and every definition
     * is dropped when the map is freed. */
    if (map->prepared) {
        S_apply_placement(parser, map);
    }

    if (map->oom) {
        parser->oom = true;
    }

    markdown_core_unlink_footnotes_map(map);
    markdown_core_map_free(map);
}

// Before calling `markdown_core_map_free` on a map with `markdown_core_footnotes`, first
// unlink all of the footnote nodes before freeing their memory.
//
// Sometimes, two (unused) footnote nodes can end up referencing each other,
// which as they get freed up by calling `markdown_core_map_free` -> `footnote_free` ->
// etc, can lead to a use-after-free error.
//
// Better to `unlink` every footnote node first, setting their next, prev, and
// parent pointers to NULL, and only then walk thru & free them up.
void markdown_core_unlink_footnotes_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    markdown_core_map_entry *next;

    ref = map->refs;
    while (ref) {
        next = ref->next;
        if (((markdown_core_footnote *)ref)->node) {
            markdown_core_node_unlink(((markdown_core_footnote *)ref)->node);
        }
        ref = next;
    }
}
