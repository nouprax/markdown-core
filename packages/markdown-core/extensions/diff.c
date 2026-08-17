#include <string.h>

#include "document_internal.h"

#include <node.h>

// THE FRONTIER DIFF: how ids hand over across an append. A tick re-derives
// the tail of the tree from the held partial line — the run of children each
// open block gained since the last publish, and the children a definition
// flip gave a unit — and pairs it against the run it replaces, so a reactive
// UI's `ForEach(id:)` reattaches row state to the right row and a node that
// did not change keeps its revision. It answers ONE decision, for each node
// in the new run: which node in the old run it IS. Everything settled keeps
// its identity by being the same object; nothing else in the tree is looked
// at.
//
// A DIFF OF THE SAME TWO RUNS IS THE SAME DIFF, whatever else is going on:
// the answer depends on the two runs and nothing outside them.
//
// Children are paired with a prefix/suffix sweep on the refined child lists:
// leading children pair front-to-back, trailing children back-to-front, the
// middle left between them still pairs positionally by type, and the residue
// retires (old) or is minted fresh (new). A kind change is a retirement and
// a creation, never a pairing (5.2). The sweeps read each subtree's hash
// (node.h), which is why a run is stamped before it is paired and stamped
// nowhere else. This is not an alignment: when one tick changes a run at
// both ends — a definition flip inserting a node in the middle AND the tail
// growing in the same chunk — the middle stops at the first kind change and
// unchanged siblings past it are minted afresh; the same bytes delivered so
// that the two land in different ticks keep every id. The bound the plan
// asks for is per tick, and an alignment would be O(run²) at the wall.
//
// Every walk here is iterative with an explicit heap stack: adversarial
// inputs nest tens of thousands of levels deep, which native recursion does
// not survive (especially under sanitizer instrumentation).

typedef struct {
    markdown_core_chain *chain; /* where identity is counted from */
    markdown_core_mem *mem;     /* the generation being built */
    uint64_t new_rev;
    bool failed;
} diff_ctx;

static void mint_subtree(diff_ctx *ctx, markdown_core_node *root) {
    markdown_core_node *node = root;

    // Ids preorder, which for a wholly new subtree is document order.
    for (;;) {
        node->id = ctx->chain->next_id++;
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

// Pushes a frame and computes its prefix/suffix pairing plan over two child
// runs. `old` and `nw` are the pair those runs hang under, and are what the
// frame classifies on exit; for THE FRONTIER they are NULL — the runs hang
// under one and the same spine block, which the caller classifies from the
// verdict — so the frame pairs and classifies the runs and nothing above.
static bool diff_plan(
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
    // EVERY id in it onto the next paragraph's content, and re-stamped every
    // revision in the suffix. Under `ForEach(id:)` that is per-row focus,
    // scroll offset and in-flight animation attaching to the wrong
    // paragraph, and a consumer pruning on revision re-renders the whole
    // tail.
    //
    // On the hash the prefix stops at the first child that genuinely
    // differs, which leaves the suffix its budget, and what falls out between
    // them is the change. The residual middle still pairs positionally by raw
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
    // changed would fall in the middle and be retired plus recreated -- the
    // identity that `ForEach(id:)` reattaches row state to would be
    // destroyed by the very change to the row it exists to survive. A KIND
    // change is a genuine retirement (5.2), so the run stops there.
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

    if (nw) {
        nw->id = old->id;
    }
    return true;
}

// Pushes a pair: its plan is over the two complete child lists.
static bool diff_push(diff_ctx *ctx, diff_stack *stack, markdown_core_node *old, markdown_core_node *nw) {
    return diff_plan(ctx, stack, old, nw, old->first_child, child_count_raw(old), nw->first_child, child_count_raw(nw));
}

// Runs the matching machine after diff_plan has pushed the root frame, and
// answers that frame's verdict: whether anything under it changed.
static bool diff_run(diff_ctx *ctx, diff_stack *stack) {
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
            // Unpaired old children retire silently: deletion has no record
            // to write, and their ids are simply never minted again. The
            // cursor still walks past them to reach the suffix run.
            for (size_t i = 0; i < top->middle_old; i++) {
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
        // is checked here (once) since node fields never change mid-walk. A
        // frontier frame has no pair of its own: its verdict is its runs'.
        bool changed = top->child_list_changed || top->descendant_changed;
        if (top->nw) {
            changed = changed || markdown_core_ast_projection_changed(top->old, top->nw);
            if (changed) {
                top->nw->last_changed_rev = ctx->new_rev;
            } else {
                top->nw->last_changed_rev = top->old->last_changed_rev;
            }
        }

        child_result = changed;
        have_result = true;
        stack->length--;
    }
    return child_result;
}

void markdown_core_diff_mint(markdown_core_chain *chain, markdown_core_node *root, uint64_t rev) {
    diff_ctx ctx = {chain, NULL, rev, false};
    mint_subtree(&ctx, root);
}

// THE FRONTIER. A warm tick keeps every settled node — same object, same id,
// same revision — and re-creates only what lives past the open spine's saved
// youngest children: the tentative subtree the previous close had minted, and
// whatever the feed and this close appended in its place. Those two runs are
// what this pairs — hash sweeps front and back, positional middle by type,
// mint the residue, classify each pair by its own fields and its children —
// so an unchanged node keeps its revision and an empty append moves nothing.
// The runs hang under one spine block rather than under a pair, so the frame
// they are planned in has no pair to classify; its verdict — did the runs
// change — is what the caller stamps the block with.
//
// The retired run owns its bytes (the retract saw to that), so classifying a
// pair by projection reads nothing that has moved.
bool markdown_core_diff_frontier(
    markdown_core_chain *chain,
    markdown_core_mem *mem,
    markdown_core_node *retired,
    markdown_core_node *fresh,
    const markdown_core_node *fresh_end,
    uint64_t rev,
    bool *changed
) {
    diff_ctx ctx = {chain, mem, rev, false};
    diff_stack stack = {NULL, 0, 0, mem};
    size_t n_old = 0;
    size_t n_new = 0;
    markdown_core_node *node;

    for (node = retired; node; node = node->next) {
        n_old++;
    }
    for (node = fresh; node && node != fresh_end; node = node->next) {
        n_new++;
    }
    /* Accumulates: a caller diffs a block's inserted and appended runs in
     * two calls, and either changing changes the block. */
    if (n_new == 0 && n_old == 0) {
        return true;
    }
    if (diff_plan(&ctx, &stack, NULL, NULL, retired, n_old, fresh, n_new)) {
        if (diff_run(&ctx, &stack)) {
            *changed = true;
        }
    }
    if (stack.frames) {
        mem->free(mem, stack.frames);
    }
    return !ctx.failed;
}
