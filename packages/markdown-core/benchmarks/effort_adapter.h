/* Instantiated against the real production functions, never copied algorithms.
 * Descriptor conversion and result conversion remain within the measured edge. */
#include "effort_runner.h"
#include <stdlib.h>

/* Only the intrusive ownership relation is observed. Native records live in a
 * caller-owned array and are not complete AST nodes with semantic payloads.
 * Allocation and tag initialization are charged, not prepared for free. */
static void effort_owners(effort_state *state) {
    size_t count = (size_t)state->length / 4;
    EFFORT_NODE *nodes = calloc(count, sizeof(*nodes));
    state->native_nodes = nodes;
    if (!nodes) {
        state->failed = 1;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        EFFORT_INIT_NODE(nodes + i);
        if (i) {
            EFFORT_ATTACH(nodes + effort_read_index(state->input + i * 4), nodes + i);
        }
    }
}

void bench_effort_observe_owners(effort_state *state) {
    EFFORT_NODE *nodes = state->native_nodes;
    size_t count = (size_t)state->length / 4;
    if (!nodes) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        EFFORT_NODE *links[] = {nodes[i].parent, nodes[i].prev, nodes[i].next, nodes[i].first_child,
                                nodes[i].last_child};
        for (size_t j = 0; j < 5; j++) {
            // The tested primitive may only link entries of this array.
            uintptr_t address = (uintptr_t)links[j], first = (uintptr_t)nodes;
            if (links[j] &&
                (address < first || address - first >= count * sizeof(*nodes) || (address - first) % sizeof(*nodes))) {
                state->failed = 1;
                return;
            }
            effort_write_index(state->data + i * 20 + j * 4,
                               links[j] ? (uint32_t)((address - first) / sizeof(*nodes)) : UINT32_MAX);
        }
    }
    state->size = state->length * 5;
    state->data[state->size] = 0;
}

EFFORT_NOINLINE void bench_effort_operation(effort_state *state, effort_op operation) {
    if (operation == EFFORT_OWNERS) {
        effort_owners(state);
        return;
    }
    if (operation == EFFORT_CLOSER) {
        effort_scan(state);
        return;
    }
    EFFORT_BUFFER buffer = EFFORT_VIEW(state);
    switch (operation) {
    case EFFORT_COPY:
        EFFORT_SET(&buffer, state->input, state->length);
        break;
    case EFFORT_TRIM:
        EFFORT_TRIM_BUFFER(&buffer);
        break;
    case EFFORT_UNESCAPE:
        EFFORT_UNESCAPE_BUFFER(&buffer);
        break;
    case EFFORT_WHITESPACE:
        EFFORT_NORMALIZE(&buffer);
        break;
    case EFFORT_CODE:
        S_normalize_code(&buffer);
        break;
    case EFFORT_CLOSER:
    case EFFORT_OWNERS:
        break;
    }
    state->size = buffer.size;
}
