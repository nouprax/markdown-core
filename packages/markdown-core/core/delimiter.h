#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include <stddef.h>
#include <stdint.h>

#include "concrete_records.h"
#include "markdown-core-extension-api.h"

/*
 * Delimiter records live in a relocatable arena. Every relationship is an
 * integer id (zero is null), so growing the arena never invalidates a stack
 * edge or leaks an address into an extension callback.
 *
 * An id packs the record's 1-based slot ordinal above a small slot
 * generation: ((ordinal) << GENERATION_BITS) | generation. Truncation
 * reclaims slots and bumps their generation, so an id that outlives its
 * record resolves to NULL instead of aliasing whatever reused the slot.
 * The ordinal sits in the high bits because ids are ordered: every ordered
 * id comparison in the engine compares ids of distinct slots, and there
 * the ordinal dominates. Zero stays the null id (the minimum live id is
 * 1 << GENERATION_BITS). The generation is a nibble and wraps: sixteen
 * reclaim cycles of one slot re-issue a generation, so the check is a
 * hardening net over the chain clamps and the diagnostics validator, not
 * a replacement for either.
 */
typedef uint32_t markdown_core_delimiter_id;

#define MARKDOWN_CORE_DELIMITER_ID_GENERATION_BITS 4u
#define MARKDOWN_CORE_DELIMITER_ID_GENERATION_MASK 0xFu
/* The last slot ordinal an id can encode, minus one so the search floor
 * sentinel (count + 1) always packs without wrapping. A push past this
 * refuses with OOM — the same fail-closed shape as an unallocatable
 * arena — instead of minting an id whose ordinal aliases another slot. */
#define MARKDOWN_CORE_DELIMITER_MAX_RECORDS                                                                            \
    ((size_t)(((markdown_core_delimiter_id)1 << (32u - MARKDOWN_CORE_DELIMITER_ID_GENERATION_BITS)) - 2))

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
    /* 1-based index into the unit's concrete capture — every push appends
     * a record through the capture required at begin, so this is never 0
     * on a live record. The patch key for reduce-time consumption. */
    uint32_t capture_index;
    unsigned char can_open;
    unsigned char can_close;
    unsigned char active;
    /* The SLOT's generation, not the record's: it survives the record
     * wipe on push, bumps when truncation reclaims the slot, and is the
     * low nibble of every id minted for the slot. Slot state carried
     * in-record so the generation check in record_at touches the cache
     * line it is about to return. */
    unsigned char generation;
} markdown_core_delimiter_record;

/* `count` is a raw record count; `tail` is a packed id. The two spaces
 * meet in mark validation, which requires the tail's decoded ordinal to
 * equal the count and the tail's generation to still match its slot. */
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
    /* The current inline unit's concrete capture: every push appends one
     * delimiter-run record through it, and the RUN/ENDPOINTS reductions
     * patch their consumption back. Required at begin, so NULL only
     * before the engine's first unit. */
    markdown_core_concrete_capture *capture;
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

void markdown_core_delimiter_engine_init(markdown_core_delimiter_engine *engine, markdown_core_mem *mem);
/* Starts an independent inline unit while retaining arena allocations.
 * Returns INVALID unless the previous unit has been fully processed back
 * to the empty mark and a concrete capture is supplied. Lane storage is
 * allocated (or grown, if the parser gained rules between documents)
 * here, never later: OOM leaves the engine on its previous unit's lane
 * table, and a successful begin is the one writer of `lane_count`, so
 * lane_count > 0 implies lanes != NULL on every downstream path. */
markdown_core_delimiter_result markdown_core_delimiter_engine_begin(
    markdown_core_delimiter_engine *engine,
    size_t lane_count,
    markdown_core_concrete_capture *capture
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

/* The newest live opener on the binding's open stack, 0 when none. The
 * stack tolerates tombstones (records a reduction removed while they were
 * stacked); this pops them lazily with path compression, which is why the
 * engine is not const. */
markdown_core_delimiter_id markdown_core_delimiter_engine_last_open(
    markdown_core_delimiter_engine *engine,
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
/* Provided by the diagnostics driver (the invariant suite), not by the
 * engine: every mutating operation re-validates the engine in diagnostics
 * builds and reports a violation here with a tag naming the mutation, so
 * a broken chain fails deterministically at the mutation site instead of
 * as a distant dereference. Linking delimiter.c with the diagnostics
 * define means supplying this symbol. */
void markdown_core_delimiter_engine_invariant_failed(const char *site);
#endif

#endif
