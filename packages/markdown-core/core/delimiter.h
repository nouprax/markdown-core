#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include <stddef.h>
#include <stdint.h>

#include "markdown-core-extension-api.h"

/*
 * Delimiter records live in a relocatable arena. Every relationship is an
 * integer id (zero is null), so growing the arena never invalidates a stack
 * edge or leaks an address into an extension callback.
 */
typedef uint32_t markdown_core_delimiter_id;

typedef struct markdown_core_delimiter_binding {
    markdown_core_extension *extension;
    const markdown_core_delimiter_rule *rule;
    markdown_core_inline_from_delim_func reduce;
    size_t lane;
    uint16_t local_kind;
} markdown_core_delimiter_binding;

typedef struct markdown_core_inline_attachment markdown_core_inline_attachment;

struct markdown_core_inline_attachment {
    markdown_core_inline_attachment *next;
    markdown_core_extension *extension;
    markdown_core_delimiter_binding *rules;
    size_t rule_count;
};

typedef struct {
    markdown_core_inline_attachment **items;
    size_t count;
    size_t capacity;
} markdown_core_inline_dispatch;

typedef struct {
    markdown_core_delimiter_binding **items;
    size_t count;
    size_t capacity;
} markdown_core_inline_close_dispatch;

typedef struct markdown_core_inline_config {
    markdown_core_mem *mem;
    markdown_core_inline_attachment *attachments;
    markdown_core_inline_attachment *attachments_tail;
    markdown_core_inline_dispatch dispatch[256];
    markdown_core_inline_dispatch seam_dispatch[256];
    markdown_core_inline_close_dispatch close_dispatch[256];
    size_t extension_rule_count;
    int8_t special_chars[256];
    int8_t seam_barrier_chars[256];
    int8_t skip_chars[256];
} markdown_core_inline_config;

typedef struct {
    markdown_core_delimiter_id tail;
    markdown_core_delimiter_id open_top;
    markdown_core_delimiter_id floor[3];
    uint32_t floor_epoch;
} markdown_core_delimiter_lane;

typedef struct {
    markdown_core_delimiter_id previous;
    markdown_core_delimiter_id next;
    markdown_core_delimiter_id previous_rule;
    markdown_core_delimiter_id next_rule;
    markdown_core_delimiter_id push_previous_rule;
    markdown_core_delimiter_id open_top_before;
    markdown_core_delimiter_id previous_open;
    const markdown_core_delimiter_binding *binding;
    markdown_core_node *marker;
    markdown_core_bufsize source_start;
    markdown_core_bufsize source_end;
    markdown_core_bufsize original_length;
    markdown_core_bufsize remaining_length;
    uint64_t claim_order;
    unsigned char can_open;
    unsigned char can_close;
    unsigned char active;
} markdown_core_delimiter_record;

typedef struct {
    uint32_t count;
    markdown_core_delimiter_id tail;
} markdown_core_delimiter_mark;

#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
/*
 * Deterministic work counters for the standalone engine invariant suite.
 * Production builds do not contain this state or any counter updates.
 */
typedef struct {
    uint64_t pushes;
    uint64_t peak_live_records;
    uint64_t capacity_growths;
    uint64_t process_calls;
    uint64_t opener_candidate_visits;
    uint64_t reductions;
    uint64_t unlinks;
    uint64_t run_bytes_consumed;
    uint64_t truncate_visits;
    uint64_t reclaimed_records;
} markdown_core_delimiter_diagnostics;
#endif

typedef struct {
    markdown_core_mem *mem;
    markdown_core_delimiter_record *records;
    markdown_core_delimiter_lane *lanes;
    size_t count;
    size_t capacity;
    size_t lane_count;
    size_t lane_capacity;
    markdown_core_delimiter_id tail;
    uint64_t last_claim_order;
    uint32_t process_epoch;
#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
    markdown_core_delimiter_diagnostics diagnostics;
#endif
} markdown_core_delimiter_engine;

markdown_core_inline_config *markdown_core_inline_config_new(
    markdown_core_mem *mem,
    const int8_t base_special_chars[256],
    const int8_t base_skip_chars[256]
);
void markdown_core_inline_config_free(markdown_core_inline_config *config);

markdown_core_delimiter_result markdown_core_inline_attachment_prepare(
    markdown_core_inline_config *config,
    markdown_core_extension *extension,
    markdown_core_inline_attachment **prepared
);
void markdown_core_inline_attachment_discard(
    markdown_core_inline_config *config,
    markdown_core_inline_attachment *attachment
);
void markdown_core_inline_attachment_commit(
    markdown_core_inline_config *config,
    markdown_core_inline_attachment *attachment
);
markdown_core_inline_attachment *markdown_core_inline_config_find_attachment(
    const markdown_core_inline_config *config,
    const markdown_core_extension *extension
);

void markdown_core_delimiter_engine_init(
    markdown_core_delimiter_engine *engine,
    markdown_core_mem *mem,
    size_t lane_count
);
/* Starts an independent inline unit while retaining arena allocations.
 * Returns INVALID unless the previous unit has been fully processed back to
 * the empty mark. Lane storage grows lazily if the parser gained rules
 * between documents. */
markdown_core_delimiter_result markdown_core_delimiter_engine_begin(
    markdown_core_delimiter_engine *engine,
    size_t lane_count
);
void markdown_core_delimiter_engine_free(markdown_core_delimiter_engine *engine);

markdown_core_delimiter_mark markdown_core_delimiter_engine_mark(const markdown_core_delimiter_engine *engine);

markdown_core_delimiter_result markdown_core_delimiter_engine_push(
    markdown_core_delimiter_engine *engine,
    const markdown_core_delimiter_binding *binding,
    int can_open,
    int can_close,
    markdown_core_node *marker,
    markdown_core_bufsize source_start,
    markdown_core_bufsize source_end,
    uint64_t claim_order
);

markdown_core_delimiter_id markdown_core_delimiter_engine_last_open(
    const markdown_core_delimiter_engine *engine,
    const markdown_core_delimiter_binding *binding
);
uint64_t markdown_core_delimiter_engine_claim_order(
    const markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_id id
);

markdown_core_delimiter_result markdown_core_delimiter_engine_process(
    markdown_core_delimiter_engine *engine,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    markdown_core_delimiter_mark mark
);
markdown_core_delimiter_result markdown_core_delimiter_engine_truncate(
    markdown_core_delimiter_engine *engine,
    markdown_core_delimiter_mark mark
);

#ifdef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
const markdown_core_delimiter_diagnostics *markdown_core_delimiter_engine_diagnostics(
    const markdown_core_delimiter_engine *engine
);
int markdown_core_delimiter_engine_validate(const markdown_core_delimiter_engine *engine);
#endif

#endif
