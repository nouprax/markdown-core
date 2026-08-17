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
// leading children pair front-to-back, trailing children back-to-front; the
// middle left between them is ALIGNED — its identical subtrees anchored by a
// longest common subsequence over (type, hash), under a budget so a run
// rewritten wholesale is not squared — and between anchors it still pairs
// positionally by type; the residue retires (old) or is minted fresh (new).
// A kind change is a retirement and a creation, never a pairing (5.2). The
// sweeps and the alignment read each subtree's hash (node.h), which is why
// a run is stamped before it is paired and stamped nowhere else. So the
// same bytes pair the same way whether one tick or two delivered them: a
// definition flip in the middle of a run and the tail growing in the same
// chunk keep every unchanged sibling's id.
//
// Every walk here is iterative with an explicit heap stack: adversarial
// inputs nest tens of thousands of levels deep, which native recursion does
// not survive (especially under sanitizer instrumentation).

typedef struct {
    markdown_core_chain *chain; /* where identity is counted from */
    markdown_core_mem *mem;     /* the generation being built */
    uint64_t new_rev;
    bool failed;
    void *scratch; /* the alignment's table, grown as needed, freed with the walk */
    size_t scratch_capacity;
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

// One step of a frame's plan over two child runs: `pairs` positional pairs
// (each pushed as a frame of its own), then `retire` old children that leave
// silently, then `mint` new children minted whole.
typedef struct diff_step {
    uint32_t pairs;
    uint32_t retire;
    uint32_t mint;
} diff_step;

// A plan is a prefix step, the middle's steps and a suffix step; a middle
// with no anchor is one step, so a frame holds its plan inline and reaches
// for the heap only when the alignment found anchors.
#define DIFF_STEPS_INLINE 4

// One in-flight (old, new) pair of the iterative matching machine.
typedef struct diff_frame {
    markdown_core_node *old;
    markdown_core_node *nw;
    markdown_core_node *oc; // paired-children cursors
    markdown_core_node *wc;
    diff_step inline_steps[DIFF_STEPS_INLINE];
    diff_step *heap_steps; // NULL while the plan fits inline
    size_t step_count;
    size_t step_index;   // the next step to load
    uint32_t pairs_left; // of the step loaded
    uint32_t retire_left;
    uint32_t mint_left;
    bool descendant_changed;
    bool child_list_changed;
} diff_frame;

static diff_step *frame_steps(diff_frame *frame) { return frame->heap_steps ? frame->heap_steps : frame->inline_steps; }

typedef struct diff_stack {
    diff_frame *frames;
    size_t length;
    size_t capacity;
    markdown_core_mem *mem;
} diff_stack;

// Releases what a walk still holds — every frame's heap plan and the frame
// array — whether the walk finished or failed part way.
static void diff_stack_release(diff_stack *stack) {
    size_t i;
    for (i = 0; i < stack->length; i++) {
        if (stack->frames[i].heap_steps) {
            stack->mem->free(stack->mem, stack->frames[i].heap_steps);
        }
    }
    if (stack->frames) {
        stack->mem->free(stack->mem, stack->frames);
    }
    stack->frames = NULL;
    stack->length = 0;
    stack->capacity = 0;
}

// THE ALIGNMENT'S BUDGET, in table cells. The middle the sweeps leave is
// aligned by a longest common subsequence over identical subtrees when its
// table fits this many cells, and pairs positionally alone when it does not.
// A frame's alignment then costs at most this many cell steps on top of the
// walk of its runs, and the tick's bound stays what the plan asks for: linear
// in the frontier, not squared at the wall. Runs are the children of one
// block; a middle wider than 64 × 64 identical-subtree candidates is a run
// rewritten wholesale, where an alignment has little left to find.
#define DIFF_ALIGN_BUDGET 4096

static bool same_subtree(const markdown_core_node *a, const markdown_core_node *b) {
    return a->type == b->type && a->subtree_hash == b->subtree_hash;
}

// Pairs a gap positionally by raw type from its start, for as far as the
// types agree; answers how many pairs.
static uint32_t gap_positional_pairs(
    const markdown_core_node *o,
    const markdown_core_node *w,
    size_t n_old,
    size_t n_new
) {
    size_t limit = n_old < n_new ? n_old : n_new;
    size_t i;
    for (i = 0; i < limit && o->type == w->type; i++) {
        o = o->next;
        w = w->next;
    }
    return (uint32_t)i;
}

// The scratch an alignment of an m × n middle works in: the two runs as
// arrays, the anchor pairs it answers (old index, new index — before the
// table, so both stay aligned), and the table itself.
typedef struct diff_align {
    markdown_core_node **olds;
    markdown_core_node **news;
    uint32_t *anchors;
    uint16_t *table;
} diff_align;

static bool align_scratch(diff_ctx *ctx, size_t m, size_t n, diff_align *out) {
    size_t fewer = m < n ? m : n;
    size_t need =
        (m + n) * sizeof(markdown_core_node *) + fewer * 2 * sizeof(uint32_t) + (m + 1) * (n + 1) * sizeof(uint16_t);
    if (need > ctx->scratch_capacity) {
        void *grown = ctx->mem->realloc(ctx->mem, ctx->scratch, need);
        if (!grown) {
            ctx->failed = true;
            return false;
        }
        ctx->scratch = grown;
        ctx->scratch_capacity = need;
    }
    out->olds = (markdown_core_node **)ctx->scratch;
    out->news = out->olds + m;
    out->anchors = (uint32_t *)(out->news + n);
    out->table = (uint16_t *)(out->anchors + fewer * 2);
    return true;
}

// Aligns an m × n middle: answers the number of anchors, written into
// `align->anchors` as (old, new) index pairs in order. The LCS is over
// identical subtrees, and its length is at most min(m, n) ≤ 64 under the
// budget, so a 16-bit table cell holds it.
static size_t align_middle(diff_align *align, markdown_core_node *o, markdown_core_node *w, size_t m, size_t n) {
    size_t width = n + 1;
    size_t i;
    size_t j;
    size_t k;
    for (i = 0; i < m; i++, o = o->next) {
        align->olds[i] = o;
    }
    for (j = 0; j < n; j++, w = w->next) {
        align->news[j] = w;
    }
    for (j = 0; j <= n; j++) {
        align->table[j] = 0;
    }
    for (i = 1; i <= m; i++) {
        align->table[i * width] = 0;
        for (j = 1; j <= n; j++) {
            if (same_subtree(align->olds[i - 1], align->news[j - 1])) {
                align->table[i * width + j] = (uint16_t)(align->table[(i - 1) * width + j - 1] + 1);
            } else {
                uint16_t up = align->table[(i - 1) * width + j];
                uint16_t left = align->table[i * width + j - 1];
                align->table[i * width + j] = up > left ? up : left;
            }
        }
    }
    // Backtrack, writing the anchors last to first.
    k = align->table[m * width + n];
    i = m;
    j = n;
    while (k > 0) {
        if (same_subtree(align->olds[i - 1], align->news[j - 1]) &&
            align->table[i * width + j] == (uint16_t)(align->table[(i - 1) * width + j - 1] + 1)) {
            k--;
            align->anchors[2 * k] = (uint32_t)(i - 1);
            align->anchors[2 * k + 1] = (uint32_t)(j - 1);
            i--;
            j--;
        } else if (align->table[(i - 1) * width + j] >= align->table[i * width + j - 1]) {
            i--;
        } else {
            j--;
        }
    }
    return align->table[m * width + n];
}

// Pushes a frame and computes its pairing plan over two child runs. `old`
// and `nw` are the pair those runs hang under, and are what the frame
// classifies on exit; for THE FRONTIER they are NULL — the runs hang under
// one and the same spine block, which the caller classifies from the
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
    diff_step *steps;
    diff_align align = {NULL, NULL, NULL, NULL};
    size_t pairable;
    size_t prefix = 0;
    size_t suffix = 0;
    size_t middle_old;
    size_t middle_new;
    size_t anchors = 0;
    size_t step_count;
    size_t at;
    size_t k;

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
    // them is the change.
    pairable = n_old < n_new ? n_old : n_new;
    while (prefix < pairable && same_subtree(o, w)) {
        prefix++;
        o = o->next;
        w = w->next;
    }
    while (suffix < pairable - prefix && same_subtree(o_end, w_end)) {
        suffix++;
        o_end = o_end->prev;
        w_end = w_end->prev;
    }
    middle_old = n_old - prefix - suffix;
    middle_new = n_new - prefix - suffix;

    // WHAT THE SWEEPS LEAVE IS ALIGNED, THEN PAIRED. A change at both ends
    // of one run — a definition flip inserting a node in the middle while
    // the tail grew in the same chunk — leaves neither sweep any budget, and
    // a middle paired positionally alone would mint every unchanged sibling
    // past the first kind change afresh, so that the same bytes kept every
    // id when the two changes came in different ticks and lost them when
    // they came in one. So the middle's identical subtrees are anchored
    // first, by a longest common subsequence over (type, hash) under the
    // budget, and what lies between anchors pairs positionally by raw type
    // from its start for as far as the types agree — a node whose own text
    // changed keeps its identity, a KIND change is a genuine retirement
    // (5.2) — and the residue retires or is minted.
    if (middle_old > 0 && middle_new > 0 && middle_old * middle_new <= DIFF_ALIGN_BUDGET) {
        if (!align_scratch(ctx, middle_old, middle_new, &align)) {
            return false;
        }
        anchors = align_middle(&align, o, w, middle_old, middle_new);
    }

    step_count = 3 + 2 * anchors;
    frame = &stack->frames[stack->length++];
    memset(frame, 0, sizeof(*frame));
    frame->old = old;
    frame->nw = nw;
    frame->oc = o_start;
    frame->wc = w_start;
    if (step_count > DIFF_STEPS_INLINE) {
        frame->heap_steps = (diff_step *)stack->mem->calloc(stack->mem, step_count, sizeof(diff_step));
        if (!frame->heap_steps) {
            stack->length--;
            ctx->failed = true;
            return false;
        }
    }
    steps = frame_steps(frame);
    frame->step_count = step_count;
    steps[0].pairs = (uint32_t)prefix;
    // The middle: a gap before each anchor, the anchor, and the gap after the
    // last — each gap paired positionally from its start.
    at = 1;
    {
        size_t oi = 0;
        size_t wi = 0;
        for (k = 0; k <= anchors; k++) {
            size_t gap_old = (k < anchors ? align.anchors[2 * k] : middle_old) - oi;
            size_t gap_new = (k < anchors ? align.anchors[2 * k + 1] : middle_new) - wi;
            uint32_t p = gap_positional_pairs(o, w, gap_old, gap_new);
            size_t step;
            steps[at].pairs = p;
            steps[at].retire = (uint32_t)(gap_old - p);
            steps[at].mint = (uint32_t)(gap_new - p);
            if (steps[at].retire || steps[at].mint) {
                frame->child_list_changed = true;
            }
            at++;
            for (step = 0; step < gap_old; step++) {
                o = o->next;
            }
            for (step = 0; step < gap_new; step++) {
                w = w->next;
            }
            oi += gap_old;
            wi += gap_new;
            if (k < anchors) {
                steps[at].pairs = 1;
                at++;
                o = o->next;
                w = w->next;
                oi++;
                wi++;
            }
        }
    }
    steps[at].pairs = (uint32_t)suffix;

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
            top->pairs_left--;
            have_result = false;
        }
        // Load the next step once this one is spent.
        while (top->pairs_left == 0 && top->retire_left == 0 && top->mint_left == 0 &&
               top->step_index < top->step_count) {
            const diff_step *step = &frame_steps(top)[top->step_index++];
            top->pairs_left = step->pairs;
            top->retire_left = step->retire;
            top->mint_left = step->mint;
        }
        if (top->pairs_left > 0) {
            // The frame pointer may dangle after a push reallocates, so read
            // the pair first.
            markdown_core_node *oc = top->oc;
            markdown_core_node *wc = top->wc;
            if (!diff_push(ctx, stack, oc, wc)) {
                break;
            }
            continue;
        }
        if (top->retire_left > 0) {
            // Unpaired old children retire silently: deletion has no record
            // to write, and their ids are simply never minted again. The
            // cursor still walks past them.
            top->oc = top->oc->next;
            top->retire_left--;
            continue;
        }
        if (top->mint_left > 0) {
            mint_subtree(ctx, top->wc);
            top->wc = top->wc->next;
            top->mint_left--;
            continue;
        }

        // Exit: every child is resolved; classify this pair. Field equality
        // is checked here (once) since node fields never change mid-walk. A
        // frontier frame has no pair of its own: its verdict is its runs'.
        {
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
        }
        if (top->heap_steps) {
            stack->mem->free(stack->mem, top->heap_steps);
            top->heap_steps = NULL;
        }
        stack->length--;
    }
    return child_result;
}

void markdown_core_diff_mint(markdown_core_chain *chain, markdown_core_node *root, uint64_t rev) {
    diff_ctx ctx = {chain, NULL, rev, false, NULL, 0};
    mint_subtree(&ctx, root);
}

// THE FRONTIER. A warm tick keeps every settled node — same object, same id,
// same revision — and re-creates only what lives past the open spine's saved
// youngest children: the tentative subtree the previous close had minted, and
// whatever the feed and this close appended in its place. Those two runs are
// what this pairs — hash sweeps front and back, the middle aligned, mint the
// residue, classify each pair by its own fields and its children —
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
    diff_ctx ctx = {chain, mem, rev, false, NULL, 0};
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
    diff_stack_release(&stack);
    if (ctx.scratch) {
        mem->free(mem, ctx.scratch);
    }
    return !ctx.failed;
}
