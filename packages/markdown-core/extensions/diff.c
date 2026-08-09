#include <string.h>

#include "document_internal.h"

#include <node.h>

// THE DIFF: what changed between the previous committed tree and a freshly
// parsed one. It answers both of the requirements that need it — which node
// is which across a commit, so a reactive UI's `ForEach(id:)` reattaches row
// state to the right row, and which nodes changed, so an imperative consumer
// can update locally. Both fall out of ONE decision: for each node in the new
// tree, which node in the old tree it IS.
//
// It was called `adopt`, after its mechanism — the new tree adopting the old
// tree's identities. The name hid the invariant. A DIFF OF THE SAME TWO TREES
// IS THE SAME DIFF, whatever else is going on; under the old name it did not
// sound wrong that the answer depended on how much of the document the caller
// let the matcher see, and it does.
//
// Children are paired with a prefix/suffix sweep on the refined child lists:
// leading children pair front-to-back, trailing children back-to-front, and
// the unpaired middle is reported as removed (old) plus added (new). A kind
// change is a retirement and a creation, never a pairing (5.2).
//
// **The pairing test is RAW TYPE, and that is a known defect.** Two paragraphs
// pair regardless of their content, and the prefix sweep may consume the whole
// pairable run before the suffix sweep is given any budget — so inserting one
// paragraph at the front of a run of same-kind siblings re-points every id in
// it onto the next node's content. Under `ForEach(id:)` that is per-row focus,
// scroll offset and in-flight animation attaching to the wrong paragraph. The
// fix is to pair on equality and to compute prefix and suffix independently;
// it is not done here yet.
//
// Parser-only storage owners have already been eliminated by refinement, so
// this tree is the canonical tree: every node receives an id and normal
// changed-versus-bubbled classification applies uniformly.
//
// Every walk here is iterative with an explicit heap stack: adversarial
// inputs nest tens of thousands of levels deep, which native recursion does
// not survive (especially under sanitizer instrumentation).

typedef struct {
    markdown_core_document *session;
    markdown_core_delta *changes;
    uint64_t new_rev;
    bool failed;
} diff_ctx;

typedef enum { REC_ADDED, REC_REMOVED, REC_CHANGED, REC_BUBBLED } rec_kind;

static void record(diff_ctx *ctx, rec_kind kind, const markdown_core_node *node) {
    markdown_core_id_array *array;
    if (!ctx->changes) {
        return;
    }
    switch (kind) {
    case REC_ADDED:
        array = &ctx->changes->added;
        break;
    case REC_REMOVED:
        array = &ctx->changes->removed;
        break;
    case REC_CHANGED:
        array = &ctx->changes->changed;
        break;
    default:
        array = &ctx->changes->bubbled;
        break;
    }
    if (!markdown_core_id_array_push(array, node->id)) {
        ctx->failed = true;
    }
}

static void mint_subtree(diff_ctx *ctx, markdown_core_node *root) {
    markdown_core_node *node = root;
    for (;;) {
        node->id = ctx->session->next_id++;
        node->last_changed_rev = ctx->new_rev;
        record(ctx, REC_ADDED, node);
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
}

static void record_removed_subtree(diff_ctx *ctx, const markdown_core_node *root) {
    const markdown_core_node *node = root;
    for (;;) {
        record(ctx, REC_REMOVED, node);
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
}

static size_t child_count_raw(const markdown_core_node *node) {
    size_t count = 0;
    const markdown_core_node *child;
    for (child = node->first_child; child; child = child->next) {
        count++;
        if (child == node->last_child) {
            break;
        }
    }
    return count;
}

static markdown_core_node *child_run_last(markdown_core_node *first, size_t count) {
    markdown_core_node *last = first;
    size_t i;
    if (count == 0) {
        return NULL;
    }
    for (i = 1; i < count && last; i++) {
        last = last->next;
    }
    return last;
}

// One in-flight (old, new) pair of the iterative matching machine.
typedef struct diff_frame {
    markdown_core_node *old;
    markdown_core_node *nw;
    markdown_core_node *oc; // paired-children cursors
    markdown_core_node *wc;
    size_t prefix_left;
    size_t middle_old;
    size_t middle_new;
    size_t suffix_left;
    bool middle_done;
    bool direct_changed;
    bool descendant_changed;
    bool child_list_changed;
} diff_frame;

typedef struct diff_stack {
    diff_frame *frames;
    size_t length;
    size_t capacity;
    markdown_core_mem *mem;
} diff_stack;

// Pushes a pair whose child runs are already bounded and computes its
// prefix/suffix pairing plan. Ordinary descendant pushes pass complete child
// lists; the incremental inline seam may reserve an already-materialized
// prefix before entering this machine.
static bool diff_push_bounded(
    diff_ctx *ctx,
    diff_stack *stack,
    markdown_core_node *old,
    markdown_core_node *nw,
    markdown_core_node *o_start,
    size_t n_old,
    markdown_core_node *w_start,
    size_t n_new
) {
    markdown_core_node *o = o_start;
    markdown_core_node *w = w_start;
    markdown_core_node *o_end;
    markdown_core_node *w_end;
    diff_frame *frame;
    size_t pairable;
    size_t prefix = 0;
    size_t suffix = 0;

    if ((n_old != 0 && !o_start) || (n_new != 0 && !w_start)) {
        ctx->failed = true;
        return false;
    }
    o_end = child_run_last(o_start, n_old);
    w_end = child_run_last(w_start, n_new);
    if ((n_old != 0 && !o_end) || (n_new != 0 && !w_end)) {
        ctx->failed = true;
        return false;
    }
    if (stack->length == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 256;
        diff_frame *grown = (diff_frame *)stack->mem->realloc(stack->mem, stack->frames, capacity * sizeof(*grown));
        if (!grown) {
            ctx->failed = true;
            return false;
        }
        stack->frames = grown;
        stack->capacity = capacity;
    }

    pairable = n_old < n_new ? n_old : n_new;
    while (prefix < pairable && o->type == w->type) {
        prefix++;
        o = o->next;
        w = w->next;
    }

    while (suffix < pairable - prefix && o_end->type == w_end->type) {
        suffix++;
        o_end = o_end->prev;
        w_end = w_end->prev;
    }

    frame = &stack->frames[stack->length++];
    frame->old = old;
    frame->nw = nw;
    frame->oc = o_start;
    frame->wc = w_start;
    frame->prefix_left = prefix;
    frame->middle_old = n_old - prefix - suffix;
    frame->middle_new = n_new - prefix - suffix;
    frame->suffix_left = suffix;
    frame->middle_done = false;
    frame->direct_changed = false;
    frame->descendant_changed = false;
    frame->child_list_changed = (n_old != n_new) || (prefix + suffix < pairable);

    nw->id = old->id;
    return true;
}

// Complete-node entry: derives the full child domains, including the
// incremental seam's reserved old prefix.
static bool diff_push(diff_ctx *ctx, diff_stack *stack, markdown_core_node *old, markdown_core_node *nw) {
    size_t n_old = child_count_raw(old);
    size_t n_new = child_count_raw(nw);
    markdown_core_node *o = old->first_child;

    // An inline seam (user_data = offset + 1, set by the commit pipeline)
    // reserves the old leaf's prefix children — one Text and one break per
    // seam line — for transplant: they survive as-is, so pairing starts
    // past them and they never enter the removal records. The count is
    // derived from the seam bytes, which both contents share.
    if (nw->user_data) {
        markdown_core_bufsize seam = (markdown_core_bufsize)((uintptr_t)nw->user_data - 1);
        markdown_core_bufsize i;
        size_t reserved = 0;
        for (i = 0; i < seam; i++) {
            if (nw->content.ptr[i] == '\n') {
                reserved += 2;
            }
        }
        for (; reserved > 0; reserved--) {
            if (!o) {
                ctx->failed = true;
                return false;
            }
            o = o->next;
            n_old--;
        }
    }
    return diff_push_bounded(ctx, stack, old, nw, o, n_old, nw->first_child, n_new);
}

// Runs the one matching machine after either a complete-node or bounded-root
// initializer has pushed its first frame. Descendant pairs always enter
// through diff_push and therefore keep complete-node semantics.
static void diff_run(diff_ctx *ctx, diff_stack *stack) {
    bool child_result = false;
    bool have_result = false;

    while (stack->length > 0 && !ctx->failed) {
        diff_frame *top = &stack->frames[stack->length - 1];

        if (have_result) {
            top->descendant_changed |= child_result;
            top->oc = top->oc->next;
            top->wc = top->wc->next;
            if (top->prefix_left > 0) {
                top->prefix_left--;
            } else {
                top->suffix_left--;
            }
            have_result = false;
        }

        if (top->prefix_left > 0 || (top->middle_done && top->suffix_left > 0)) {
            // The frame pointer may dangle after a push reallocates, so read
            // the pair first.
            markdown_core_node *oc = top->oc;
            markdown_core_node *wc = top->wc;
            if (!diff_push(ctx, stack, oc, wc)) {
                break;
            }
            continue;
        }

        if (!top->middle_done) {
            for (size_t i = 0; i < top->middle_old; i++) {
                record_removed_subtree(ctx, top->oc);
                top->oc = top->oc->next;
            }
            for (size_t i = 0; i < top->middle_new; i++) {
                mint_subtree(ctx, top->wc);
                top->wc = top->wc->next;
            }
            top->middle_done = true;
            continue;
        }

        // Exit: every child is resolved; classify this pair. Field equality
        // is checked here (once) since node fields never change mid-walk.
        bool direct = top->direct_changed || !markdown_core_ast_fields_equal(top->old, top->nw);
        if (direct || top->child_list_changed) {
            top->nw->last_changed_rev = ctx->new_rev;
            record(ctx, REC_CHANGED, top->nw);
        } else if (top->descendant_changed) {
            top->nw->last_changed_rev = ctx->new_rev;
            record(ctx, REC_BUBBLED, top->nw);
        } else {
            top->nw->last_changed_rev = top->old->last_changed_rev;
        }

        child_result = direct || top->child_list_changed || top->descendant_changed;
        have_result = true;
        stack->length--;
    }
}

static void diff_pair(diff_ctx *ctx, markdown_core_node *old_root, markdown_core_node *new_root) {
    diff_stack stack = {NULL, 0, 0, ctx->session->mem};

    if (diff_push(ctx, &stack, old_root, new_root)) {
        diff_run(ctx, &stack);
    }
    if (stack.frames) {
        ctx->session->mem->free(ctx->session->mem, stack.frames);
    }
}

bool markdown_core_diff_trees_inline_domain(
    markdown_core_document *session,
    markdown_core_node *old_owner,
    markdown_core_node *staged_owner,
    uint64_t new_rev,
    markdown_core_delta *changes,
    uint64_t *owner_revision
) {
    diff_ctx ctx = {session, changes, new_rev, false};

    diff_pair(&ctx, old_owner, staged_owner);
    if (ctx.failed) {
        return false;
    }
    *owner_revision = staged_owner->last_changed_rev;
    return true;
}

bool markdown_core_diff_trees(
    markdown_core_document *session,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev,
    markdown_core_delta *changes
) {
    diff_ctx ctx = {session, changes, new_rev, false};

    if (!old_root) {
        mint_subtree(&ctx, new_root);
        return !ctx.failed;
    }

    // Roots are both documents; pair them directly.
    diff_pair(&ctx, old_root, new_root);
    return !ctx.failed;
}

bool markdown_core_document_record_removed(
    markdown_core_document *session,
    const markdown_core_node *root,
    markdown_core_delta *changes
) {
    diff_ctx ctx = {session, changes, 0, false};
    record_removed_subtree(&ctx, root);
    return !ctx.failed;
}
