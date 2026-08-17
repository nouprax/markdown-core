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
// middle left between them is ALIGNED — its identical subtrees matched by
// EDIT DISTANCE, so the alignment costs the change and not the run — and
// between the matches it still pairs positionally by type; the residue
// retires (old) or is minted fresh (new). A kind change is a retirement and
// a creation, never a pairing (5.2). The sweeps and the alignment read each
// subtree's hash (node.h), which is why a run is stamped before it is
// paired and stamped nowhere else. So the same bytes pair the same way
// whether one tick or two delivered them — a definition flip at the front
// of a run and the tail growing in the same chunk keep the unchanged
// siblings between, however long the run is — until the two sides differ by
// more than the alignment's bounds below, where a run is being rewritten
// rather than edited and the middle pairs positionally alone.
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

// THE ALIGNMENT COSTS THE CHANGE, NOT THE RUN. The middle the sweeps leave
// is aligned by Myers' greedy diff over identical subtrees: it walks the
// edit graph outward by EDIT DISTANCE, so two sides that differ by D
// elementary edits are aligned in O((m + n) · D) whatever m and n are —
// and a stream's middle differs by a handful of edits however long the
// paragraph is. A table over the middle would have been O(m · n), which for
// a paragraph of 32 lines already costs more than the parse that produced
// it, so identity would have depended on the paragraph's length at every
// size; it now depends on it only past the bounds below.
//
// THE GROWTH IS FREE; THE CHANGES ARE WHAT IS BOUNDED. A run that gained
// children must spend one move per child gained whatever else happened to
// it, so a bound on the distance alone would charge a tick for the SIZE of
// its chunk: a hundred lines arriving beside a definition flip would
// exhaust it, and a run nothing had rewritten would pair positionally and
// lose every id. The walk is therefore indexed by the moves that CONSUME
// THE SHORTER SIDE — at most DIFF_ALIGN_CHANGES of them, one per child of
// the shorter run left unmatched — while the moves that consume the longer
// side are unbounded and free. That is also what makes the walk's memory
// O(distance × changes) rather than O(distance²), so a chunk may bring any
// number of children without the alignment growing quadratically.
//
// Two ceilings hold the worst case: DIFF_ALIGN_MAX_EDITS caps the distance
// outright, and a frame never spends more than DIFF_ALIGN_WORK subtree
// compares, so the wider a run is the shallower the look it gets. Past the
// reach the middle pairs positionally, as it did before there was an
// alignment.
#define DIFF_ALIGN_CHANGES 64
#define DIFF_ALIGN_MAX_EDITS 4096
#define DIFF_ALIGN_WORK (1 << 22)

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

// The scratch one alignment works in: the two runs as arrays (`few` is the
// shorter, `many` the longer), the walk's frontier — how far into the
// shorter run each number of unmatched children reaches — that frontier as
// it stood at every distance so the path can be walked back, and the SNAKES
// the path is made of: runs of identical subtrees, {old index, new index,
// length}.
typedef struct diff_align {
    markdown_core_node **few;
    markdown_core_node **many;
    bool swapped; // the shorter run is the NEW one
    int32_t *v;   // indexed by unmatched children, 0..changes
    int32_t *trace;
    size_t changes; // the most children of the shorter run that may go unmatched
    uint32_t *snakes;
} diff_align;

static bool align_scratch(diff_ctx *ctx, size_t m, size_t n, size_t reach, size_t changes, diff_align *out) {
    size_t row = changes + 1;
    size_t need = (m + n) * sizeof(markdown_core_node *) + (row + (reach + 1) * row) * sizeof(int32_t) +
                  (reach + 2) * 3 * sizeof(uint32_t);
    if (need > ctx->scratch_capacity) {
        /* From the CHAIN's allocator, never the generation's: the buffer
         * outlives the tick that grew it, and a generation's may be an
         * arena released whole with that generation. */
        markdown_core_mem *mem = ctx->chain->mem;
        void *grown = mem->realloc(mem, ctx->scratch, need);
        if (!grown) {
            ctx->failed = true;
            return false;
        }
        ctx->scratch = grown;
        ctx->scratch_capacity = need;
    }
    out->few = (markdown_core_node **)ctx->scratch;
    out->many = out->few + (m < n ? m : n);
    out->v = (int32_t *)(out->few + m + n);
    out->trace = out->v + row;
    out->changes = changes;
    out->snakes = (uint32_t *)(out->trace + (reach + 1) * row);
    return true;
}

// Aligns an m × n middle within `reach` moves: answers how many snakes the
// path is made of, written into `align->snakes` in document order, or 0 when
// the two sides are further apart than that.
//
// The walk extends every snake as far as it goes before spending another
// move, which is what makes a subtree with several identical candidates —
// every soft break hashes the same, so does a repeated word — pair with the
// EARLIEST one: a run grows at its end, so the change the sweeps could not
// reach is a flip at the front, and the trailing gap must be left holding
// the node being typed into against what it became. Paired with a later
// candidate instead, the growing text's id hops onto the line after it.
static size_t align_middle(
    diff_align *align,
    markdown_core_node *o,
    markdown_core_node *w,
    size_t m,
    size_t n,
    size_t reach
) {
    size_t few_length = m < n ? m : n;
    size_t many_length = m < n ? n : m;
    size_t row = align->changes + 1;
    size_t d;
    size_t i;

    align->swapped = n < m;
    for (i = 0; i < m; i++, o = o->next) {
        (align->swapped ? align->many : align->few)[i] = o;
    }
    for (i = 0; i < n; i++, w = w->next) {
        (align->swapped ? align->few : align->many)[i] = w;
    }
    for (d = 0; d <= reach; d++) {
        size_t r = d < align->changes ? d : align->changes;
        // Descending, so each step still reads the previous distance's
        // frontier where it needs it.
        for (;; r--) {
            int32_t x;
            int32_t y;
            // Reach this many unmatched children by the cheaper of the two
            // moves that lead to it: one more child of the LONGER run taken
            // (free), or one more child of the shorter run left behind.
            if (d == 0) {
                x = 0;
            } else if (r == 0 || (r != d && align->v[r - 1] < align->v[r])) {
                x = align->v[r];
            } else {
                x = align->v[r - 1] + 1;
            }
            y = x + (int32_t)(d - 2 * r);
            while (x < (int32_t)few_length && y < (int32_t)many_length && same_subtree(align->few[x], align->many[y])) {
                x++;
                y++;
            }
            align->v[r] = x;
            align->trace[d * row + r] = x;
            if (x >= (int32_t)few_length && y >= (int32_t)many_length) {
                // Both sides are consumed: walk the path back, writing its
                // snakes last to first into the end of the array, then move
                // them to its front.
                size_t room = d + 1;
                size_t written = 0;
                int32_t px = (int32_t)few_length;
                size_t back;
                size_t at = r;
                for (back = d; back > 0; back--) {
                    const int32_t *prev = align->trace + (back - 1) * row;
                    bool free_move = at == 0 || (at != back && prev[at - 1] < prev[at]);
                    size_t from = free_move ? at : at - 1;
                    int32_t prev_x = prev[from];
                    int32_t snake_x = free_move ? prev_x : prev_x + 1;
                    if (px > snake_x) {
                        uint32_t *snake = align->snakes + 3 * (room - ++written);
                        snake[0] = (uint32_t)snake_x;
                        snake[1] = (uint32_t)(snake_x + (int32_t)(back - 2 * at));
                        snake[2] = (uint32_t)(px - snake_x);
                    }
                    px = prev_x;
                    at = from;
                }
                if (px > 0) {
                    /* A path that opens with a match. Every caller sweeps
                     * the identical leading children off first, so this
                     * cannot happen where the middle is what is aligned; it
                     * is the algorithm's, not the caller's, and stays. */
                    uint32_t *snake = align->snakes + 3 * (room - ++written);
                    snake[0] = 0;
                    snake[1] = 0;
                    snake[2] = (uint32_t)px;
                }
                if (written && room > written) {
                    memmove(align->snakes, align->snakes + 3 * (room - written), written * 3 * sizeof(uint32_t));
                }
                if (align->swapped) {
                    // The walk read the new run as the shorter one; the
                    // plan is written in the old run's terms.
                    for (i = 0; i < written; i++) {
                        uint32_t swap = align->snakes[3 * i];
                        align->snakes[3 * i] = align->snakes[3 * i + 1];
                        align->snakes[3 * i + 1] = swap;
                    }
                }
                return written;
            }
            if (r == 0) {
                break;
            }
        }
    }
    return 0;
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
    diff_align align = {NULL, NULL, false, NULL, NULL, 0, NULL};
    size_t pairable;
    size_t prefix = 0;
    size_t suffix = 0;
    size_t middle_old;
    size_t middle_new;
    size_t snakes = 0;
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
    // of one run — a definition flip inserting a node at its front while the
    // tail grew in the same chunk — leaves neither sweep any budget, and a
    // middle paired positionally alone would mint every unchanged sibling
    // past the first kind change afresh, so that the same bytes kept every
    // id when the two changes came in different ticks and lost them when
    // they came in one. So the middle's identical subtrees are matched first
    // (over type and hash, by edit distance), and what lies between the
    // matches pairs positionally by raw type from its start for as far as
    // the types agree — a node whose own text changed keeps its identity, a
    // KIND change is a genuine retirement (5.2) — and the residue retires or
    // is minted.
    if (middle_old > 0 && middle_new > 0) {
        /* How far the walk may go: the difference in length, which every
         * path pays and no bound may refuse, plus the changes allowed on top
         * of it — then cut to the memory ceiling, to the work this frame may
         * spend, and to the distance the two sides can possibly be apart. A
         * reach short of the difference in length cannot reach the end at
         * all, and that middle pairs positionally without a walk. */
        size_t spread = middle_old > middle_new ? middle_old - middle_new : middle_new - middle_old;
        size_t budget = DIFF_ALIGN_WORK / (middle_old + middle_new);
        size_t reach = spread + 2 * DIFF_ALIGN_CHANGES;
        if (reach > DIFF_ALIGN_MAX_EDITS) {
            reach = DIFF_ALIGN_MAX_EDITS;
        }
        if (reach > budget) {
            reach = budget;
        }
        if (reach > middle_old + middle_new) {
            reach = middle_old + middle_new;
        }
        /* A reach short of the difference in length cannot reach the end at
         * all, and that middle pairs positionally without a walk. What is
         * left over the difference pays for the children of the shorter run
         * that go unmatched, two moves each. */
        if (reach >= spread) {
            size_t changes = (reach - spread) / 2;
            size_t fewer = middle_old < middle_new ? middle_old : middle_new;
            if (changes > fewer) {
                changes = fewer;
            }
            if (!align_scratch(ctx, middle_old, middle_new, reach, changes, &align)) {
                return false;
            }
            snakes = align_middle(&align, o, w, middle_old, middle_new, reach);
        }
    }

    step_count = 3 + 2 * snakes;
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
    // The middle: a gap before each snake, the snake, and the gap after the
    // last — each gap paired positionally from its start.
    at = 1;
    {
        size_t oi = 0;
        size_t wi = 0;
        for (k = 0; k <= snakes; k++) {
            size_t gap_old = (k < snakes ? align.snakes[3 * k] : middle_old) - oi;
            size_t gap_new = (k < snakes ? align.snakes[3 * k + 1] : middle_new) - wi;
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
            if (k < snakes) {
                size_t length = align.snakes[3 * k + 2];
                steps[at].pairs = (uint32_t)length;
                at++;
                for (step = 0; step < length; step++) {
                    o = o->next;
                    w = w->next;
                }
                oi += length;
                wi += length;
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

void markdown_core_diff_scratch_release(markdown_core_chain *chain) {
    if (chain && chain->pairing_scratch) {
        chain->mem->free(chain->mem, chain->pairing_scratch);
        chain->pairing_scratch = NULL;
        chain->pairing_scratch_size = 0;
    }
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
    diff_ctx ctx = {chain, mem, rev, false, chain->pairing_scratch, chain->pairing_scratch_size};
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
    /* Back to the chain, grown: the next tick's pairing starts where this
     * one left off, and only the chain's release frees it. */
    chain->pairing_scratch = ctx.scratch;
    chain->pairing_scratch_size = ctx.scratch_capacity;
    return !ctx.failed;
}
