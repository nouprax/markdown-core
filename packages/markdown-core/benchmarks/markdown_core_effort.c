/* Including the production TU makes its private normalizer reachable without
 * a public API or a benchmark-only branch in the parser. The archive's code.o
 * is not extracted: this TU supplies the same external definitions. */
#include "../elements/code.c"
#include "effort_runner.h"

static void effort_scan(effort_state *state) {
    markdown_core_inline_state native = {0};
    native.input.data = (unsigned char *)state->input;
    native.input.len = state->length;
    native.pos = state->start;
    native.backticks = state->cache;
    native.backtick_capacity = 80;
    state->result = markdown_core_inline_scan_to_closing_backticks(&native, state->ticks);
    state->position = native.pos;
    state->scanned = native.scanned_for_backticks;
}

#define EFFORT_BUFFER markdown_core_strbuf
#define EFFORT_VIEW(s) {(s)->data, (s)->capacity, (s)->size, 0}
#define EFFORT_SET markdown_core_strbuf_set
#define EFFORT_TRIM_BUFFER markdown_core_strbuf_trim
#define EFFORT_UNESCAPE_BUFFER markdown_core_strbuf_unescape
#define EFFORT_NORMALIZE markdown_core_strbuf_normalize_whitespace
#define EFFORT_NODE markdown_core_node
#define EFFORT_INIT_NODE(n) ((n)->kind = MARKDOWN_CORE_NODE_CALLOUT)
#define EFFORT_ATTACH(parent, child) markdown_core_node_attach_validated(parent, child, NULL)
#include "effort_adapter.h"
