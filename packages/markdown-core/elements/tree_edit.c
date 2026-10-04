#include <stdint.h>
#include <stdlib.h>

#include "alloc.h"
#include "ast_internal.h"

/* THE EDIT PASS (docs/plans/2026-09-29-incremental-parsing.md, 5.2): a batch
 * of edits applied to the tree parsed from the text before it, as
 * `ts_tree_edit` applies an edit to a tree-sitter tree. Each edit descends
 * from the root along the nodes whose range, from the start of their lead to
 * their end plus their reach, meets or touches it; each such node takes the
 * extent of its image and is marked changed. Every other node keeps its
 * extent, which is relative, and is not visited. A node the edit leaves with
 * no byte is anchored nowhere: no new node is matched to it and no lookup
 * continues it (blocks.c), so the pass does not descend into it.
 *
 * The session holds its tree once between edits: the previous parse released
 * every node its result does not hold, so the pass changes the tree in place.
 *
 * Edits apply last to first, each in the coordinates of the text before the
 * batch, which an edit later in the text does not move. Edits that touch,
 * one ending where the next starts, apply as one: no byte survives between
 * them, so a node starting in the first starts at the image of the first
 * byte after the last. */

/* One replacement: bytes [start, end) of the text become `size` bytes. */
typedef struct {
    int64_t start, end, size;
} edit_range;

/* A node whose relations the edit meets: the node itself, from its own old
 * start and the image of that start, or a definition's body, whose children
 * run from where the definition starts. */
typedef struct {
    markdown_core_node *node;
    int64_t old_origin, new_origin;
    bool body;
} edit_task;

/* The edit in hand, the nodes whose relations it may still meet, each node
 * the batch moved, with its extent before the batch, and each holder whose
 * children tree the batch unsealed, sealed once when the batch is done. */
typedef struct {
    edit_range edit;
    edit_task *tasks;
    size_t count, capacity;
    markdown_core_moved *moved;
    size_t moved_count, moved_capacity;
    markdown_core_node **unsealed;
    size_t unsealed_count, unsealed_capacity;
    bool ok;
} edit_pass;

static bool edit_reserve(void **values, size_t *capacity, size_t count, size_t size) {
    if (count < *capacity) {
        return true;
    }
    size_t grown = *capacity ? 2 * *capacity : 32;
    void *resized;
    if (grown > SIZE_MAX / size || !(resized = markdown_core_realloc(*values, grown * size))) {
        return false;
    }
    *values = resized;
    *capacity = grown;
    return true;
}

static bool edit_push(edit_pass *pass, markdown_core_node *node, int64_t old_origin, int64_t new_origin, bool body) {
    if (!edit_reserve((void **)&pass->tasks, &pass->capacity, pass->count, sizeof(*pass->tasks))) {
        return false;
    }
    pass->tasks[pass->count++] = (edit_task){node, old_origin, new_origin, body};
    return true;
}

/* The image of a node [start, end): its start is the image of its first byte
 * that survives the edit, or the edit's start when none does, and its end the
 * image of its last surviving byte's end, or its start when none survives. An
 * empty node, which has no byte, is where its start's image is. */
static inline int64_t image_start(const edit_range *edit, int64_t start, int64_t end) {
    if (start < edit->start) {
        return start;
    }
    if (start >= edit->end) {
        return start + edit->size - (edit->end - edit->start);
    }
    return end > edit->end ? edit->start + edit->size : edit->start;
}

static inline int64_t image_end(const edit_range *edit, int64_t start, int64_t end, int64_t image) {
    if (start == end) {
        return image;
    }
    if (end <= edit->start) {
        return end;
    }
    if (end > edit->end) {
        return end + edit->size - (edit->end - edit->start);
    }
    return start < edit->start ? edit->start : image;
}

/* `node`, which the edit moves, is marked changed. The first edit of the
 * batch to move a node records its extent before the batch, which is what a
 * new node matched to it compares (ast.c, publish_same); false when that
 * record could not be stored. A node left with no byte is anchored nowhere,
 * so no new node is matched to it and it needs no record. */
static inline bool edit_mark(edit_pass *pass, markdown_core_node *node, bool anchored) {
    if (anchored && !(node->flags & MARKDOWN_CORE_NODE__CHANGED)) {
        if (!edit_reserve((void **)&pass->moved, &pass->moved_capacity, pass->moved_count, sizeof(*pass->moved))) {
            return false;
        }
        pass->moved[pass->moved_count++] = (markdown_core_moved){node, node->where.extent};
    }
    node->flags |= MARKDOWN_CORE_NODE__CHANGED;
    return true;
}

/* `node` takes its image, relative to `image`, the new end of the node before
 * it in its relation, which ended at `previous`, and is marked changed;
 * `image` moves to its new end. */
static inline bool edit_image(edit_pass *pass, markdown_core_node *node, int64_t previous, int64_t *image) {
    const edit_range *edit = &pass->edit;
    int64_t start = previous + node->where.extent.lead, end = start + node->where.extent.span;
    int64_t new_start = image_start(edit, start, end), new_end = image_end(edit, start, end, new_start);
    if (!edit_mark(pass, node, new_end != new_start)) {
        return false;
    }
    node->where.extent = (markdown_core_extent){(int32_t)(new_start - *image), (uint32_t)(new_end - new_start)};
    *image = new_end;
    return true;
}

/* Whether the edit meets `node`, after a node that ended at `previous`: its
 * range from `from`, or from its start when that is before, to its end plus
 * its reach. A node's range starts where its lead does, at `previous`; a
 * definition body's starts where its children's leads do. A node the edit
 * does not meet keeps its extent, and `image` moves to its end, which the
 * edit leaves where it was. A node it meets takes its image; `pass->ok` turns
 * false when its record could not be stored. */
static inline bool edit_node(edit_pass *pass, markdown_core_node *node, int64_t previous, int64_t from,
                             int64_t *image) {
    const edit_range *edit = &pass->edit;
    int64_t start = previous + node->where.extent.lead, end = start + node->where.extent.span;
    if ((start < from ? start : from) > edit->end || end + node->reach < edit->start) {
        *image = end;
        return false;
    }
    pass->ok = edit_image(pass, node, previous, image);
    return true;
}

/* The children [first, last) of `holder`, a relation whose first node's lead
 * runs from `old_origin`, now at `new_origin`. A run is stepped over whole
 * when its range plus reach ends before the edit, and the walk stops at the
 * first node that starts after it: the children of one relation are in
 * source order, and every node after that one keeps its lead. Under `body`
 * the children are a definition's bodies, each a relation whose first node's
 * lead runs from `old_origin` (canonical-ast.md): the edit meets every body
 * from the one it reaches on, and each is pushed with that origin. Any other
 * node the edit meets is pushed with its own start. The runs on the path to
 * a changed node are unsealed, and sealed again when the batch is done
 * (5.1): an earlier edit steps over runs before it, which a later edit does
 * not move. `*met` says whether the edit met a node; false when storage ran
 * out. */
static bool edit_children(edit_pass *pass, markdown_core_node *holder, size_t first, size_t last, int64_t old_origin,
                          int64_t new_origin, bool body, bool *met) {
    const edit_range *edit = &pass->edit;
    markdown_core_run *path[MARKDOWN_CORE_RUN_TIERS];
    uint8_t at[MARKDOWN_CORE_RUN_TIERS];
    int depth = 1;
    path[0] = holder->children;
    at[0] = 0;
    /* The lengths of the children before `index`, and where the relation's
     * first node's lead starts less the lengths before it, once the walk has
     * reached `first`. */
    int64_t prefix = 0, base = old_origin, image = new_origin;
    size_t index = 0;
    *met = false;
    while (depth) {
        markdown_core_run *run = path[depth - 1];
        if (at[depth - 1] == run->count) {
            depth--;
            continue;
        }
        void *entry = run->entries[at[depth - 1]++];
        if (index == first) {
            base = old_origin - prefix;
        }
        if (run->tier) {
            markdown_core_run *below = entry;
            if (index + below->total <= first) {
                prefix += below->length;
                index += below->total;
                continue;
            }
            if (index >= last) {
                break;
            }
            if (index >= first) {
                int64_t lead = base + prefix;
                if (lead > edit->end && !body) {
                    break;
                }
                if (lead + below->length + below->reach < edit->start) {
                    prefix += below->length;
                    index += below->total;
                    image = base + prefix;
                    continue;
                }
            }
            path[depth] = below;
            at[depth] = 0;
            depth++;
            continue;
        }
        markdown_core_node *node = entry;
        if (index < first) {
            prefix += node->where.extent.lead + (int64_t)node->where.extent.span;
            index++;
            continue;
        }
        if (index++ >= last) {
            break;
        }
        int64_t previous = base + prefix;
        if (previous > edit->end && previous + node->where.extent.lead > edit->end && !body) {
            break;
        }
        prefix += node->where.extent.lead + (int64_t)node->where.extent.span;
        int64_t old_start = previous + node->where.extent.lead;
        if (!edit_node(pass, node, previous, body ? old_origin : previous, &image)) {
            continue;
        }
        if (!pass->ok) {
            return false;
        }
        *met = true;
        int64_t new_start = image - node->where.extent.span;
        if (path[0]->sealed) {
            if (!edit_reserve((void **)&pass->unsealed, &pass->unsealed_capacity, pass->unsealed_count,
                              sizeof(*pass->unsealed))) {
                return false;
            }
            pass->unsealed[pass->unsealed_count++] = holder;
        }
        for (int i = 0; i < depth; i++) {
            path[i]->sealed = 0;
        }
        /* A definition's body holds no byte of its own to be anchored by:
         * its children are read against it by their body (canonical-ast.md),
         * so the pass descends into every body it meets. */
        if (!body && !node->where.extent.span) {
            continue;
        }
        if (!(body ? edit_push(pass, node, old_origin, new_origin, true)
                   : edit_push(pass, node, old_start, new_start, false))) {
            return false;
        }
    }
    return true;
}

/* The relations of the task's node the edit may meet. */
static bool edit_relations(edit_pass *pass, const edit_task *task) {
    markdown_core_node *owner = task->node;
    bool met;
    if (task->body) {
        return !owner->children ||
               edit_children(pass, owner, 0, owner->children->total, task->old_origin, task->new_origin, false, &met);
    }
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    markdown_core_relations_begin(&cursor, owner);
    while (markdown_core_relations_next(&cursor, &relation)) {
        if (relation.field) {
            markdown_core_node *node = *relation.field;
            int64_t image = task->new_origin, old_start = task->old_origin + node->where.extent.lead;
            if (edit_node(pass, node, task->old_origin, task->old_origin, &image) &&
                !(pass->ok && (!node->where.extent.span ||
                               edit_push(pass, node, old_start, image - node->where.extent.span, false)))) {
                return false;
            }
            continue;
        }
        met = false;
        if (relation.holder && relation.holder->children &&
            !edit_children(pass, relation.holder, relation.start, relation.end, task->old_origin, task->new_origin,
                           false, &met)) {
            return false;
        }
        if (relation.holder != owner && met) {
            relation.holder->flags |= MARKDOWN_CORE_NODE__CHANGED;
        }
        /* A definition's bodies hold extents of their own, from where the
         * definition starts (ast.c, publish_bodies): the bodies the edit
         * meets are walked for their children, and the others are not. */
        if (owner->kind == MARKDOWN_CORE_NODE_DEFINITION) {
            return !owner->children || edit_children(pass, owner, 0, owner->children->total, task->old_origin,
                                                     task->new_origin, true, &met);
        }
    }
    return true;
}

static uint64_t moved_key(const void *entry) { return (uint64_t)(uintptr_t)((const markdown_core_moved *)entry)->node; }

/* The batch applied to the relations under `root`: the document's, whose
 * root every edit meets, or a registry's, whose entries run from 0. */
static bool edit_batch(markdown_core_node *root, bool registry, const markdown_core_byte_edit *edits, size_t count,
                       markdown_core_moved **moved, size_t *moved_count) {
    edit_pass pass = {{0, 0, 0}, NULL, 0, 0, NULL, 0, 0, NULL, 0, 0, true};
    for (size_t i = count; pass.ok && i--;) {
        pass.edit = (edit_range){(int64_t)edits[i].start, (int64_t)edits[i].end, (int64_t)edits[i].size};
        while (i && edits[i - 1].end == edits[i].start) {
            i--;
            pass.edit.start = (int64_t)edits[i].start;
            pass.edit.size += (int64_t)edits[i].size;
        }
        if (registry) {
            pass.ok = edit_push(&pass, root, 0, 0, false);
        } else {
            /* The root is always met: the document is read again whatever
             * the edit. It holds the whole text, from its first byte. */
            pass.ok = edit_mark(&pass, root, true) && edit_push(&pass, root, 0, 0, false);
            root->where.extent.span =
                (uint32_t)(root->where.extent.span + pass.edit.size - (pass.edit.end - pass.edit.start));
        }
        while (pass.ok && pass.count) {
            edit_task task = pass.tasks[--pass.count];
            pass.ok = edit_relations(&pass, &task);
        }
    }
    markdown_core_free(pass.tasks);
    for (size_t i = 0; i < pass.unsealed_count; i++) {
        markdown_core_children_seal(pass.unsealed[i]->children);
    }
    markdown_core_free(pass.unsealed);
    /* In the order of their nodes' addresses, which ast.c searches. */
    if (pass.ok && moved) {
        markdown_core_source_order order = {0};
        pass.ok =
            markdown_core_order_source_entries(&order, pass.moved, pass.moved_count, sizeof(*pass.moved), moved_key);
        markdown_core_source_order_dispose(&order);
    }
    if (!pass.ok || !moved) {
        markdown_core_free(pass.moved);
        return pass.ok;
    }
    *moved = pass.moved;
    *moved_count = pass.moved_count;
    return true;
}

bool markdown_core_tree_edit(markdown_core_node *root, const markdown_core_byte_edit *edits, size_t count,
                             markdown_core_moved **moved, size_t *moved_count) {
    return edit_batch(root, false, edits, count, moved, moved_count);
}

bool markdown_core_registry_edit(markdown_core_node *registry, const markdown_core_byte_edit *edits, size_t count) {
    return edit_batch(registry, true, edits, count, NULL, NULL);
}
