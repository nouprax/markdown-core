/* Instantiated against the real production functions, never copied algorithms.
 * Descriptor conversion and result conversion remain within the measured edge. */
#include "effort_runner.h"

EFFORT_NOINLINE void bench_effort_operation(effort_state *state, effort_op operation) {
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
        break;
    }
    state->size = buffer.size;
}
