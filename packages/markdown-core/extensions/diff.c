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
    markdown_core_document *document;
    markdown_core_delta *changes;
    uint64_t new_rev;
    bool failed;
} diff_ctx;

// THIS WALK EMITS THE DELTA. It used to only annotate nodes, because a second
// pass -- the footnote index diff -- could still add an ANSWERS part to a node
// this one had already classified, so nothing could be written down until both
// had spoken. That pass is gone with the part it reported, so the verdict is
// final at frame exit and the row is pushed there. A separate postorder walk
// over the whole tree, plus the scratch field it read, went with it.
static void mark(diff_ctx *ctx, markdown_core_node *node, uint32_t parts) {
    node->last_changed_rev = ctx->new_rev;
    if (ctx->changes && !markdown_core_delta_push(ctx->changes, node->id, parts)) {
        ctx->failed = true;
    }
}

static void mint_subtree(diff_ctx *ctx, markdown_core_node *root) {
    markdown_core_node *node = root;

    // Ids preorder, which for a wholly new subtree is document order.
    for (;;) {
        node->id = ctx->document->next_id++;
        node->last_changed_rev = ctx->new_rev;
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

    if (!ctx->changes) {
        return;
    }
    // Rows postorder, because a consumer building immutable values bottom-up
    // needs a node's children before the node. A created node differs from
    // absence in every part it has (9.1).
    node = root;
    while (node->first_child) {
        node = node->first_child;
    }
    for (;;) {
        if (!markdown_core_delta_push(ctx->changes, node->id, markdown_core_ast_parts_present(node))) {
            ctx->failed = true;
            return;
        }
        if (node == root) {
            return;
        }
        if (node->next) {
            node = node->next;
            while (node->first_child) {
                node = node->first_child;
            }
        } else {
            node = node->parent;
        }
    }
}

// A retired subtree is pushed where it is found: inside its former parent's
// run, and so before that parent's own row. Deletion is addressed by id, so a
// retired row needs no position of its own; what this ordering buys is that a
// consumer drops a parent's dead children before it re-reads that parent.
static void record_removed_subtree(diff_ctx *ctx, const markdown_core_node *root) {
    const markdown_core_node *node = root;
    for (;;) {
        if (ctx->changes && !markdown_core_delta_push(ctx->changes, node->id, 0)) {
            ctx->failed = true;
            return;
        }
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
    size_t middle_pair_left;
    size_t middle_old;
    size_t middle_new;
    size_t suffix_left;
    bool middle_done;
    bool descendant_changed;
    bool child_list_changed;
} diff_frame;

typedef struct diff_stack {
    diff_frame *frames;
    size_t length;
    size_t capacity;
    markdown_core_mem *mem;
} diff_stack;

// Pushes a pair and computes its prefix/suffix pairing plan over the two
// complete child lists.
static bool diff_push(diff_ctx *ctx, diff_stack *stack, markdown_core_node *old, markdown_core_node *nw) {
    markdown_core_node *o_start = old->first_child;
    markdown_core_node *w_start = nw->first_child;
    size_t n_old = child_count_raw(old);
    size_t n_new = child_count_raw(nw);
    markdown_core_node *o = o_start;
    markdown_core_node *w = w_start;
    markdown_core_node *o_end;
    markdown_core_node *w_end;
    diff_frame *frame;
    size_t pairable;
    size_t prefix = 0;
    size_t suffix = 0;
    size_t middle_old;
    size_t middle_new;
    size_t middle_pair;

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

    // THE SWEEPS PAIR IDENTICAL SUBTREES, not same-kind ones.
    //
    // On raw type they paired anything, so the prefix sweep ate the whole
    // pairable run before the suffix sweep was given a byte of budget:
    // inserting one paragraph at the front of a run of paragraphs re-pointed
    // EVERY id in it onto the next paragraph's content, and reported the whole
    // suffix as changed. Under `ForEach(id:)` that is per-row focus, scroll
    // offset and in-flight animation attaching to the wrong paragraph, and it
    // was measured at roughly half the cost of a mid-document edit -- the
    // delta carried the entire suffix.
    //
    // On the hash the prefix stops at the first child that genuinely
    // differs, which leaves the suffix its budget, and what falls out between
    // them is the edit. The residual middle still pairs positionally by raw
    // type below, so a node whose own text changed keeps its identity instead
    // of being retired and recreated.
    pairable = n_old < n_new ? n_old : n_new;
    while (prefix < pairable && o->type == w->type && o->subtree_hash == w->subtree_hash) {
        prefix++;
        o = o->next;
        w = w->next;
    }

    while (suffix < pairable - prefix && o_end->type == w_end->type && o_end->subtree_hash == w_end->subtree_hash) {
        suffix++;
        o_end = o_end->prev;
        w_end = w_end->prev;
    }

    // WHAT IS LEFT BETWEEN THE SWEEPS STILL PAIRS, positionally and by raw
    // type, for as far as the types agree. Without this a node whose own text
    // changed would fall in the middle and be reported as a retirement plus a
    // creation -- the identity that `ForEach(id:)` reattaches row state to
    // would be destroyed by editing the row. A KIND change is a genuine
    // retirement (5.2), so the run stops there.
    middle_old = n_old - prefix - suffix;
    middle_new = n_new - prefix - suffix;
    middle_pair = middle_old < middle_new ? middle_old : middle_new;
    {
        markdown_core_node *mo = o;
        markdown_core_node *mw = w;
        size_t i;
        for (i = 0; i < middle_pair; i++) {
            if (mo->type != mw->type) {
                break;
            }
            mo = mo->next;
            mw = mw->next;
        }
        middle_pair = i;
    }

    frame = &stack->frames[stack->length++];
    frame->old = old;
    frame->nw = nw;
    frame->oc = o_start;
    frame->wc = w_start;
    frame->prefix_left = prefix;
    frame->middle_pair_left = middle_pair;
    frame->middle_old = middle_old - middle_pair;
    frame->middle_new = middle_new - middle_pair;
    frame->suffix_left = suffix;
    frame->middle_done = false;
    frame->descendant_changed = false;
    // Every child paired means the list itself did not change, whatever the
    // paired children did to their own contents.
    frame->child_list_changed = frame->middle_old != 0 || frame->middle_new != 0;

    nw->id = old->id;
    return true;
}

// Runs the matching machine after diff_push has pushed the root pair.
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
            } else if (top->middle_pair_left > 0) {
                top->middle_pair_left--;
            } else {
                top->suffix_left--;
            }
            have_result = false;
        }

        if (top->prefix_left > 0 || top->middle_pair_left > 0 || (top->middle_done && top->suffix_left > 0)) {
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
        uint32_t parts = markdown_core_ast_parts_changed(top->old, top->nw);
        if (top->child_list_changed) {
            parts |= MARKDOWN_CORE_DIFF_CHILDREN;
        }
        if (top->descendant_changed) {
            parts |= MARKDOWN_CORE_DIFF_DESCENDANT;
        }
        if (parts) {
            mark(ctx, top->nw, parts);
        } else {
            top->nw->last_changed_rev = top->old->last_changed_rev;
        }

        child_result = parts != 0;
        have_result = true;
        stack->length--;
    }
}

static void diff_pair(diff_ctx *ctx, markdown_core_node *old_root, markdown_core_node *new_root) {
    diff_stack stack = {NULL, 0, 0, ctx->document->mem};

    if (diff_push(ctx, &stack, old_root, new_root)) {
        diff_run(ctx, &stack);
    }
    if (stack.frames) {
        ctx->document->mem->free(ctx->document->mem, stack.frames);
    }
}

bool markdown_core_diff_trees(
    markdown_core_document *document,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev,
    markdown_core_delta *changes
) {
    diff_ctx ctx = {document, changes, new_rev, false};

    if (!old_root) {
        // NOTHING PAIRS AGAINST NOTHING, so nothing is hashed. The hash
        // exists only to decide which children pair, and a first parse pairs
        // no children -- running it here cost 24% of a 41 MB parse to build
        // an answer that was thrown away, and it is what pushed that corpus's
        // scaling gate from 3.44x to 4.03x against a 4.0x bound.
        mint_subtree(&ctx, new_root);
        return !ctx.failed;
    }

    // Roots are both documents; pair them directly.
    diff_pair(&ctx, old_root, new_root);
    return !ctx.failed;
}
