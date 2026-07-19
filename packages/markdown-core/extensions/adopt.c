#include <string.h>

#include "session_internal.h"

#include "directive.h"

#include <node.h>

// Id adoption between the previous committed tree and a freshly parsed tree.
//
// Children are paired with a prefix/suffix sweep on the raw child lists:
// leading children whose raw types match pair up front-to-back, trailing
// children pair back-to-front, and the unpaired middle is reported as
// removed (old) plus added (new). Pairing by position and kind is what keeps
// the streaming frontier stable: an append extends the tail of the document,
// so every prefix node keeps its id, and a kind change never adopts.
//
// Directive-label wrapper nodes are facade-invisible. They pair and carry
// ids like any raw child, but they are never recorded in deltas, and a
// change inside a label marks the owning directive as `changed` (a label is
// a typed property edge of its directive, not ordinary content).
//
// Every walk here is iterative with an explicit heap stack: adversarial
// inputs nest tens of thousands of levels deep, which native recursion does
// not survive (especially under sanitizer instrumentation).

typedef struct {
    markdown_core_session *session;
    markdown_core_delta *changes;
    uint64_t new_rev;
    bool failed;
} adopt_ctx;

static bool is_wrapper(const markdown_core_node *node) { return node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL; }

typedef enum { REC_ADDED, REC_REMOVED, REC_CHANGED, REC_BUBBLED } rec_kind;

static void record(adopt_ctx *ctx, rec_kind kind, const markdown_core_node *node) {
    markdown_core_id_array *array;
    if (!ctx->changes || is_wrapper(node)) {
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

static void mint_subtree(adopt_ctx *ctx, markdown_core_node *root) {
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

static void record_removed_subtree(adopt_ctx *ctx, const markdown_core_node *root) {
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
    for (const markdown_core_node *child = node->first_child; child; child = child->next) {
        count++;
    }
    return count;
}

// One in-flight (old, new) pair of the iterative adoption machine.
typedef struct adopt_frame {
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
    bool pending_child_is_wrapper;
} adopt_frame;

typedef struct adopt_stack {
    adopt_frame *frames;
    size_t length;
    size_t capacity;
    markdown_core_mem *mem;
} adopt_stack;

// Pushes a pair and computes its pairing plan (prefix/suffix sweeps).
static bool adopt_push(adopt_ctx *ctx, adopt_stack *stack, markdown_core_node *old, markdown_core_node *nw) {
    if (stack->length == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 256;
        adopt_frame *grown = (adopt_frame *)stack->mem->realloc(stack->mem, stack->frames, capacity * sizeof(*grown));
        if (!grown) {
            ctx->failed = true;
            return false;
        }
        stack->frames = grown;
        stack->capacity = capacity;
    }

    size_t n_old = child_count_raw(old);
    size_t n_new = child_count_raw(nw);
    size_t pairable = n_old < n_new ? n_old : n_new;

    markdown_core_node *o = old->first_child;
    markdown_core_node *w = nw->first_child;
    size_t prefix = 0;
    while (prefix < pairable && o->type == w->type) {
        prefix++;
        o = o->next;
        w = w->next;
    }

    markdown_core_node *o_end = old->last_child;
    markdown_core_node *w_end = nw->last_child;
    size_t suffix = 0;
    while (suffix < pairable - prefix && o_end->type == w_end->type) {
        suffix++;
        o_end = o_end->prev;
        w_end = w_end->prev;
    }

    adopt_frame *frame = &stack->frames[stack->length++];
    frame->old = old;
    frame->nw = nw;
    frame->oc = old->first_child;
    frame->wc = nw->first_child;
    frame->prefix_left = prefix;
    frame->middle_old = n_old - prefix - suffix;
    frame->middle_new = n_new - prefix - suffix;
    frame->suffix_left = suffix;
    frame->middle_done = false;
    frame->direct_changed = false;
    frame->descendant_changed = false;
    frame->child_list_changed = (n_old != n_new) || (prefix + suffix < pairable);
    frame->pending_child_is_wrapper = false;

    nw->id = old->id;
    return true;
}

// Runs the machine over the pair (old_root, new_root).
static void adopt_pair(adopt_ctx *ctx, markdown_core_node *old_root, markdown_core_node *new_root) {
    adopt_stack stack = {NULL, 0, 0, ctx->session->mem};
    bool child_result = false;
    bool have_result = false;

    if (!adopt_push(ctx, &stack, old_root, new_root)) {
        return;
    }

    while (stack.length > 0 && !ctx->failed) {
        adopt_frame *top = &stack.frames[stack.length - 1];

        if (have_result) {
            top->descendant_changed |= child_result;
            if (child_result && top->pending_child_is_wrapper) {
                top->direct_changed = true;
            }
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
            top->pending_child_is_wrapper = is_wrapper(wc);
            if (!adopt_push(ctx, &stack, oc, wc)) {
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
        stack.length--;
    }

    if (stack.frames) {
        ctx->session->mem->free(ctx->session->mem, stack.frames);
    }
}

bool markdown_core_session_adopt(
    markdown_core_session *session,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev,
    markdown_core_delta *changes
) {
    adopt_ctx ctx = {session, changes, new_rev, false};

    if (!old_root) {
        mint_subtree(&ctx, new_root);
        return !ctx.failed;
    }

    // Roots are both documents; adopt in place.
    adopt_pair(&ctx, old_root, new_root);
    return !ctx.failed;
}

bool markdown_core_session_record_removed(
    markdown_core_session *session,
    const markdown_core_node *root,
    markdown_core_delta *changes
) {
    adopt_ctx ctx = {session, changes, 0, false};
    record_removed_subtree(&ctx, root);
    return !ctx.failed;
}
