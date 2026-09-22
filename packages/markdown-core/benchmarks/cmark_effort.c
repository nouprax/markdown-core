/* Compile the pinned production TU in this adapter, not a transcription. */
#include <inlines.c>
#include "effort_runner.h"

static void effort_scan(effort_state *state) {
    subject native = {0};
    native.input.data = (unsigned char *)state->input;
    native.input.len = state->length;
    native.pos = state->start;
    state->result = scan_to_closing_backticks(&native, state->ticks);
    state->position = native.pos;
    state->scanned = native.scanned_for_backticks;
    memcpy(state->cache, native.backticks, sizeof(state->cache));
}

#define EFFORT_BUFFER cmark_strbuf
#define EFFORT_VIEW(s) {cmark_get_default_mem_allocator(), (s)->data, (s)->capacity, (s)->size}
#define EFFORT_SET cmark_strbuf_set
#define EFFORT_TRIM_BUFFER cmark_strbuf_trim
#define EFFORT_UNESCAPE_BUFFER cmark_strbuf_unescape
#define EFFORT_NORMALIZE cmark_strbuf_normalize_whitespace
#define EFFORT_NODE cmark_node
#define EFFORT_INIT_NODE(n) ((n)->type = CMARK_NODE_BLOCK_QUOTE)
#define EFFORT_ATTACH(parent, child)                                                                                   \
    do {                                                                                                               \
        if (!cmark_node_append_child(parent, child)) {                                                                 \
            state->failed = 1;                                                                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)
#include "effort_adapter.h"
