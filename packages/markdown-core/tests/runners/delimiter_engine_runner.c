/*
 * Deterministic delimiter-engine invariant suite.
 *
 * This runner compiles delimiter.c directly with
 * MARKDOWN_CORE_DELIMITER_DIAGNOSTICS. Production libraries therefore carry
 * neither counters nor counter branches. The assertions below pin work to
 * semantic operations instead of wall-clock behavior:
 *
 *   delimiter_engine_runner --list
 *   delimiter_engine_runner --case NAME
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "concrete_records.h"
#include "delimiter.h"
#include "extension.h"

#ifndef MARKDOWN_CORE_DELIMITER_DIAGNOSTICS
#error "delimiter_engine_runner requires MARKDOWN_CORE_DELIMITER_DIAGNOSTICS"
#endif

#define DR_RECORD_COUNT 4096u
#define DR_RUN_LENGTH 8192

typedef struct {
    markdown_core_mem mem;
    uint64_t allocation_attempts;
    uint64_t calloc_calls;
    uint64_t realloc_calls;
    uint64_t free_calls;
    uint64_t fail_at;
} dr_allocator;

typedef struct {
    uint64_t calls;
    markdown_core_delimiter_match last;
} dr_reduction_log;

/* The engine requires a concrete capture at begin. The invariant suite
 * hands it a plain-libc one so the dr_allocator counters keep measuring
 * the arena alone; main() abandons it after every case. */
static void *dr_plain_calloc(markdown_core_mem *mem, size_t count, size_t size) {
    (void)mem;
    return calloc(count, size);
}
static void *dr_plain_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    (void)mem;
    return realloc(pointer, size);
}
static void dr_plain_free(markdown_core_mem *mem, void *pointer) {
    (void)mem;
    free(pointer);
}
static markdown_core_mem dr_plain_mem = {dr_plain_calloc, dr_plain_realloc, dr_plain_free};
static markdown_core_concrete_capture dr_capture;

/* The engine re-validates itself after every mutating operation and
 * reports violations here (the diagnostics link contract). Recording
 * instead of aborting keeps the reaction assertable: main() fails any
 * case that ends with an unexamined violation, and the corruption case
 * asserts the report fires at the mutation site it broke. */
static uint64_t dr_invariant_failures;
static const char *dr_invariant_site;

void markdown_core_delimiter_engine_invariant_failed(const char *site) {
    dr_invariant_failures++;
    dr_invariant_site = site;
}

static int dr_engine_start(markdown_core_delimiter_engine *engine, markdown_core_mem *mem, size_t lane_count) {
    markdown_core_delimiter_engine_init(engine, mem);
    markdown_core_concrete_capture_init(&dr_capture, &dr_plain_mem);
    return markdown_core_delimiter_engine_begin(engine, lane_count, &dr_capture) == MARKDOWN_CORE_DELIMITER_OK;
}

static uint32_t dr_xorshift(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t dr_id_ordinal(markdown_core_delimiter_id id) {
    return id >> MARKDOWN_CORE_DELIMITER_ID_GENERATION_BITS;
}

static dr_reduction_log *dr_active_log;
static markdown_core_delimiter_result dr_reduction_result;
static unsigned char dr_marker_storage;

static void *dr_calloc(markdown_core_mem *mem, size_t count, size_t size) {
    dr_allocator *allocator = (dr_allocator *)mem;
    allocator->allocation_attempts++;
    allocator->calloc_calls++;
    if (allocator->fail_at && allocator->allocation_attempts == allocator->fail_at) {
        return NULL;
    }
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    return calloc(count, size);
}

static void *dr_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    dr_allocator *allocator = (dr_allocator *)mem;
    allocator->allocation_attempts++;
    allocator->realloc_calls++;
    if (allocator->fail_at && allocator->allocation_attempts == allocator->fail_at) {
        return NULL;
    }
    return realloc(pointer, size);
}

static void dr_free(markdown_core_mem *mem, void *pointer) {
    dr_allocator *allocator = (dr_allocator *)mem;
    allocator->free_calls++;
    free(pointer);
}

static void dr_allocator_init(dr_allocator *allocator) {
    memset(allocator, 0, sizeof(*allocator));
    allocator->mem.calloc = dr_calloc;
    allocator->mem.realloc = dr_realloc;
    allocator->mem.free = dr_free;
}

static markdown_core_delimiter_result dr_reduce(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    if (dr_active_log) {
        dr_active_log->calls++;
        dr_active_log->last = *match;
    }
    return dr_reduction_result;
}

static markdown_core_bufsize dr_close_probe(
    uint16_t kind,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset
) {
    (void)kind;
    return data && offset >= 0 && offset < len ? 1 : 0;
}

static markdown_core_delimiter_binding dr_binding(const markdown_core_delimiter_rule *rule, size_t lane) {
    markdown_core_delimiter_binding binding;
    memset(&binding, 0, sizeof(binding));
    binding.rule = rule;
    binding.reduce = dr_reduce;
    binding.lane = lane;
    binding.local_kind = (uint16_t)lane;
    return binding;
}

static int dr_push(
    markdown_core_delimiter_engine *engine,
    const markdown_core_delimiter_binding *binding,
    int can_open,
    int can_close,
    markdown_core_bufsize source_start,
    markdown_core_bufsize run_length
) {
    return markdown_core_delimiter_engine_push(
               engine,
               binding,
               can_open,
               can_close,
               (markdown_core_node *)&dr_marker_storage,
               source_start,
               source_start + run_length,
               0
           ) == MARKDOWN_CORE_DELIMITER_OK;
}

static int dr_require(int condition, const char *case_name, const char *claim) {
    if (!condition) {
        fprintf(stderr, "%s: %s\n", case_name, claim);
        return 0;
    }
    return 1;
}

#define DR_REQUIRE(condition, claim)                                                                                   \
    do {                                                                                                               \
        if (!dr_require((condition), case_name, (claim))) {                                                            \
            result = -1;                                                                                               \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)

static int case_balanced_nearest_ranges(void) {
    static const char case_name[] = "balanced_nearest_ranges";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    dr_reduction_log log;
    const markdown_core_delimiter_diagnostics *diagnostics;
    markdown_core_bufsize position = 0;
    size_t i;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    for (i = 0; i < DR_RECORD_COUNT; i++) {
        DR_REQUIRE(dr_push(&engine, &binding, 1, 0, position++, 1), "opener push failed");
    }
    for (i = 0; i < DR_RECORD_COUNT; i++) {
        DR_REQUIRE(dr_push(&engine, &binding, 0, 1, position++, 1), "closer push failed");
    }
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "pre-process topology is invalid");

    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "post-process topology is invalid");
    DR_REQUIRE(engine.count == 0 && engine.tail == 0, "full process did not reclaim the arena");
    DR_REQUIRE(log.calls == DR_RECORD_COUNT, "nearest pairs did not reduce exactly once each");
    DR_REQUIRE(
        diagnostics->opener_candidate_visits == DR_RECORD_COUNT,
        "nearest search visited more than one candidate per pair"
    );
    DR_REQUIRE(diagnostics->unlinks == 2u * DR_RECORD_COUNT, "a delimiter was unlinked more than once");
    DR_REQUIRE(
        diagnostics->truncate_visits == 2u * DR_RECORD_COUNT && diagnostics->reclaimed_records == diagnostics->pushes,
        "truncate work is not one visit per pushed record"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    return result;
}

static int dr_commonmark_case(
    const char *case_name,
    markdown_core_bufsize opener_length,
    markdown_core_bufsize closer_length,
    int pairs
) {
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_COMMONMARK,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    dr_reduction_log log;
    const markdown_core_delimiter_diagnostics *diagnostics;
    markdown_core_bufsize position = 0;
    size_t i;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 1, position, opener_length), "CommonMark opener push failed");
    position += opener_length;
    for (i = 0; i < DR_RECORD_COUNT; i++) {
        DR_REQUIRE(dr_push(&engine, &binding, 0, 1, position, closer_length), "CommonMark closer push failed");
        position += closer_length;
    }
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "pre-process topology is invalid");

    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "post-process topology is invalid");
    DR_REQUIRE(
        diagnostics->opener_candidate_visits == 1,
        "a CommonMark search class revisited an exhausted opener range"
    );
    DR_REQUIRE(log.calls == (pairs ? 1u : 0u), "CommonMark rule-of-three pairing count changed");
    DR_REQUIRE(diagnostics->unlinks == DR_RECORD_COUNT + (pairs ? 1u : 0u), "CommonMark retirement count changed");
    DR_REQUIRE(
        diagnostics->reclaimed_records == diagnostics->pushes && diagnostics->truncate_visits == diagnostics->pushes,
        "CommonMark process did not reclaim each physical record exactly once"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    return result;
}

static int case_commonmark_modulo_one_floor(void) { return dr_commonmark_case("commonmark_modulo_one_floor", 2, 1, 0); }

static int case_commonmark_modulo_two_floor(void) { return dr_commonmark_case("commonmark_modulo_two_floor", 1, 2, 0); }

static int case_commonmark_modulo_zero_pairs(void) {
    return dr_commonmark_case("commonmark_modulo_zero_pairs", 1, 3, 1);
}

static int case_per_rule_isolation(void) {
    static const char case_name[] = "per_rule_isolation";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    enum { lane_count = 33 };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding bindings[lane_count];
    const markdown_core_delimiter_diagnostics *diagnostics;
    markdown_core_bufsize position = 0;
    size_t i;
    int result = 0;

    dr_allocator_init(&allocator);
    dr_active_log = NULL;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, lane_count), "engine start failed");
    for (i = 0; i < lane_count; i++) {
        bindings[i] = dr_binding(&rule, i);
    }
    for (i = 0; i < DR_RECORD_COUNT; i++) {
        DR_REQUIRE(dr_push(&engine, &bindings[0], 1, 0, position++, 1), "lane-zero opener push failed");
    }
    for (i = 0; i < DR_RECORD_COUNT; i++) {
        size_t lane = 1 + i % (lane_count - 1);
        DR_REQUIRE(dr_push(&engine, &bindings[lane], 0, 1, position++, 1), "unrelated closer push failed");
    }
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "pre-process topology is invalid");

    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "post-process topology is invalid");
    DR_REQUIRE(diagnostics->opener_candidate_visits == 0, "a closer searched delimiter records owned by another rule");
    DR_REQUIRE(diagnostics->reductions == 0, "unrelated rules formed a pair");
    DR_REQUIRE(diagnostics->unlinks == DR_RECORD_COUNT, "closer-only records were not retired once");
    DR_REQUIRE(diagnostics->reclaimed_records == diagnostics->pushes, "arena suffix was not fully reclaimed");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_mark_restore_and_reuse(void) {
    static const char case_name[] = "mark_restore_and_reuse";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    markdown_core_delimiter_mark mark;
    dr_reduction_log log;
    const markdown_core_delimiter_diagnostics *diagnostics;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");

    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "prefix opener push failed");
    mark = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 1, 1), "suffix opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 2, 1), "suffix closer push failed");
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, (markdown_core_delimiter_mark){mark.count + 1, mark.tail}) ==
            MARKDOWN_CORE_DELIMITER_INVALID,
        "forged mark was accepted"
    );
    DR_REQUIRE(
        engine.count == 3 && dr_id_ordinal(engine.tail) == 3 && markdown_core_delimiter_engine_validate(&engine),
        "forged mark changed engine state"
    );
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, mark);

    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "mark restore left invalid topology");
    DR_REQUIRE(engine.count == 1 && dr_id_ordinal(engine.tail) == 1, "mark restore did not preserve only the prefix");
    DR_REQUIRE(log.calls == 1 && log.last.opener_start == 1, "suffix pair selected the wrong opener");

    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 3, 1), "reused closer push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "reused arena left invalid topology");
    DR_REQUIRE(
        log.calls == 2 && log.last.opener_start == 0 && log.last.closer_start == 3,
        "reused handle did not pair with the preserved prefix"
    );
    DR_REQUIRE(
        diagnostics->pushes == 4 && diagnostics->reductions == 2 && diagnostics->opener_candidate_visits == 2,
        "mark/reuse work count changed"
    );
    DR_REQUIRE(
        diagnostics->reclaimed_records == diagnostics->pushes && diagnostics->truncate_visits == diagnostics->pushes,
        "mark/reuse did not visit each pushed record exactly once"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    return result;
}

static int case_stale_id_is_refused(void) {
    static const char case_name[] = "stale_id_is_refused";
    static const markdown_core_delimiter_rule ordinary_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule shared_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        ']',
        dr_close_probe,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding ordinary = dr_binding(&ordinary_rule, 0);
    markdown_core_delimiter_binding shared = dr_binding(&shared_rule, 1);
    markdown_core_delimiter_mark mark;
    markdown_core_delimiter_id stale;
    int result = 0;

    dr_allocator_init(&allocator);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 2), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &ordinary, 1, 0, 0, 1), "prefix opener push failed");
    mark = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &shared,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            1,
            2,
            7
        ) == MARKDOWN_CORE_DELIMITER_OK,
        "claimed opener push failed"
    );
    stale = markdown_core_delimiter_engine_last_open(&engine, &shared);
    DR_REQUIRE(
        stale != 0 && markdown_core_delimiter_engine_claim_order(&engine, stale) == 7,
        "live id did not resolve to its claim order"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, mark) == MARKDOWN_CORE_DELIMITER_OK,
        "restore to the prefix mark failed"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_claim_order(&engine, stale) == 0,
        "a reclaimed id resolved past the shrunk arena"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &shared,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            2,
            3,
            9
        ) == MARKDOWN_CORE_DELIMITER_OK,
        "slot-reusing push failed"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_claim_order(&engine, stale) == 0,
        "a reclaimed id resolved to the record that reused its slot"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_claim_order(
            &engine,
            markdown_core_delimiter_engine_last_open(&engine, &shared)
        ) == 9,
        "the reusing record did not resolve through its own id"
    );
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "stale-id probe left invalid topology");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_stale_mark_is_refused(void) {
    static const char case_name[] = "stale_mark_is_refused";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    markdown_core_delimiter_mark prefix;
    markdown_core_delimiter_mark stale;
    markdown_core_delimiter_mark fresh;
    dr_reduction_log log;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "prefix opener push failed");
    prefix = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 1, 1), "reclaimed opener push failed");
    stale = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, prefix) == MARKDOWN_CORE_DELIMITER_OK,
        "restore to the prefix mark failed"
    );
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 2, 1), "slot-reusing push failed");
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, stale) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a stale mark was accepted by truncate"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_process(&engine, NULL, NULL, stale) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a stale mark was accepted by process"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, (markdown_core_delimiter_mark){0, stale.tail}) ==
            MARKDOWN_CORE_DELIMITER_INVALID,
        "an empty-count mark carrying a tail was accepted"
    );
    DR_REQUIRE(
        engine.count == 2 && log.calls == 0 && markdown_core_delimiter_engine_validate(&engine),
        "a refused stale mark changed engine state"
    );
    fresh = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 3, 1), "post-refusal opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 4, 1), "post-refusal closer push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, fresh);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "fresh-mark process left invalid topology");
    DR_REQUIRE(engine.count == 2 && log.calls == 1, "an equal-shape fresh mark did not restore and reduce");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    return result;
}

static int case_generation_wrap_is_refused(void) {
    static const char case_name[] = "generation_wrap_is_refused";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    markdown_core_delimiter_mark prefix;
    markdown_core_delimiter_mark stale;
    markdown_core_bufsize position = 2;
    size_t cycle;
    int result = 0;

    dr_allocator_init(&allocator);
    dr_active_log = NULL;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "prefix opener push failed");
    prefix = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 1, 1), "reclaimed opener push failed");
    stale = markdown_core_delimiter_engine_mark(&engine);
    /* Sixteen reclaim/reuse cycles of the stale mark's slot wrap the id's
     * generation nibble back to its original value; only the mark's full
     * generation still tells the two records apart. */
    for (cycle = 0; cycle < 16; cycle++) {
        DR_REQUIRE(
            markdown_core_delimiter_engine_truncate(&engine, prefix) == MARKDOWN_CORE_DELIMITER_OK,
            "wrap-cycle restore failed"
        );
        DR_REQUIRE(dr_push(&engine, &binding, 1, 0, position++, 1), "wrap-cycle push failed");
    }
    DR_REQUIRE(engine.tail == stale.tail, "sixteen reclaims did not wrap the id nibble");
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, stale) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a wrapped stale mark was accepted by truncate"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_process(&engine, NULL, NULL, stale) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a wrapped stale mark was accepted by process"
    );
    DR_REQUIRE(
        engine.count == 2 && markdown_core_delimiter_engine_validate(&engine),
        "a refused wrapped mark changed engine state"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static markdown_core_delimiter_id dr_id_at(const markdown_core_delimiter_engine *engine, uint32_t ordinal) {
    return (markdown_core_delimiter_id)(((markdown_core_delimiter_id)ordinal
                                         << MARKDOWN_CORE_DELIMITER_ID_GENERATION_BITS) |
                                        (engine->records[ordinal - 1].generation &
                                         MARKDOWN_CORE_DELIMITER_ID_GENERATION_MASK));
}

static int case_validator_rejects_corruption(void) {
    static const char case_name[] = "validator_rejects_corruption";
    static const markdown_core_delimiter_rule ordinary_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule shared_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        ']',
        dr_close_probe,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_engine unbegun;
    markdown_core_delimiter_binding ordinary = dr_binding(&ordinary_rule, 0);
    markdown_core_delimiter_binding shared = dr_binding(&shared_rule, 1);
    markdown_core_delimiter_binding foreign_lane;
    markdown_core_delimiter_record *r1;
    markdown_core_delimiter_record *r2;
    markdown_core_delimiter_record *r3;
    markdown_core_delimiter_lane *lanes_saved;
    markdown_core_delimiter_mark full;
    size_t count_saved;
    size_t capacity_saved;
    int result = 0;

/* Break one invariant, require the validator to reject the engine, undo
 * the break, require it to pass again. The restore is part of the claim:
 * a probe that cannot restore was not the isolated corruption it says. */
#define DR_CORRUPTION(corrupt, restore, claim)                                                                         \
    do {                                                                                                               \
        corrupt;                                                                                                       \
        DR_REQUIRE(!markdown_core_delimiter_engine_validate(&engine), claim);                                          \
        restore;                                                                                                       \
        DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "restore left " claim);                           \
    } while (0)

    dr_allocator_init(&allocator);
    dr_active_log = NULL;
    DR_REQUIRE(!markdown_core_delimiter_engine_validate(NULL), "a null engine validated");
    markdown_core_delimiter_engine_init(&unbegun, &allocator.mem);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&unbegun), "a fresh engine did not validate");
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 2), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &ordinary, 1, 0, 0, 1), "first opener push failed");
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &shared,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            1,
            2,
            5
        ) == MARKDOWN_CORE_DELIMITER_OK,
        "claimed opener push failed"
    );
    DR_REQUIRE(dr_push(&engine, &ordinary, 1, 0, 2, 1), "second opener push failed");
    r1 = &engine.records[0];
    r2 = &engine.records[1];
    r3 = &engine.records[2];
    full = markdown_core_delimiter_engine_mark(&engine);
    foreign_lane = ordinary;
    foreign_lane.lane = 99;

    DR_CORRUPTION(r1->binding = NULL, r1->binding = &ordinary, "a bindingless record");
    DR_CORRUPTION(r1->binding = &foreign_lane, r1->binding = &ordinary, "a record on a lane past the table");
    DR_CORRUPTION(r1->source_end = 0, r1->source_end = 1, "an empty source range");
    DR_CORRUPTION(r1->original_length = 2, r1->original_length = 1, "a length detached from its range");
    DR_CORRUPTION(r1->remaining_length = 0, r1->remaining_length = 1, "an active record with nothing remaining");
    DR_CORRUPTION(r1->can_open = 0, r1->can_open = 1, "a record that can neither open nor close");
    DR_CORRUPTION(r2->can_close = 1, r2->can_close = 0, "a shared-close record opening and closing at once");
    DR_CORRUPTION(r2->claim_order = 0, r2->claim_order = 5, "a shared-close opener without a claim");
    DR_CORRUPTION(r1->claim_order = 3, r1->claim_order = 0, "a claim on a rule that never claims");
    DR_CORRUPTION(r2->source_start = 0, r2->source_start = 1, "overlapping source ranges");
    DR_CORRUPTION(r2->claim_order = 100, r2->claim_order = 5, "a claim past the engine's last issued order");
    DR_CORRUPTION(r1->previous = dr_id_at(&engine, 3), r1->previous = 0, "a broken active-chain prefix");
    DR_CORRUPTION(r1->next = 0, r1->next = dr_id_at(&engine, 2), "a severed forward link");
    DR_CORRUPTION(r2->next = dr_id_at(&engine, 1), r2->next = dr_id_at(&engine, 3), "a backward forward-link");
    DR_CORRUPTION(r1->generation++, r1->generation--, "links holding a retired generation");
    DR_CORRUPTION(
        r3->previous_rule = dr_id_at(&engine, 2),
        r3->previous_rule = dr_id_at(&engine, 1),
        "a rule chain crossing lanes"
    );
    DR_CORRUPTION(r3->next_rule = dr_id_at(&engine, 1), r3->next_rule = 0, "a lane tail with a next_rule");
    DR_CORRUPTION(
        engine.lanes[0].tail = dr_id_at(&engine, 1),
        engine.lanes[0].tail = dr_id_at(&engine, 3),
        "a lane tail naming an interior record"
    );
    DR_CORRUPTION(
        engine.lanes[1].open_top = dr_id_at(&engine, 1),
        engine.lanes[1].open_top = dr_id_at(&engine, 2),
        "an open stack topped by a foreign record"
    );
    DR_CORRUPTION(engine.tail = dr_id_at(&engine, 1), engine.tail = dr_id_at(&engine, 3), "a stale engine tail");
    DR_CORRUPTION(engine.last_claim_order = 0, engine.last_claim_order = 5, "an engine behind its records' claims");
    DR_CORRUPTION(engine.count = engine.capacity + 1, engine.count = 3, "a count past the arena");
    count_saved = engine.count;
    capacity_saved = engine.capacity;
    DR_CORRUPTION(
        (engine.count = MARKDOWN_CORE_DELIMITER_MAX_RECORDS + 1,
         engine.capacity = MARKDOWN_CORE_DELIMITER_MAX_RECORDS + 2),
        (engine.count = count_saved, engine.capacity = capacity_saved),
        "a count past the id space"
    );
    lanes_saved = engine.lanes;
    DR_CORRUPTION(engine.lanes = NULL, engine.lanes = lanes_saved, "a lane count without a lane table");

    /* The refusal at the id-space cap: with count pinned to the last
     * encodable ordinal, growth must refuse and the push report OOM
     * without touching the arena. */
    engine.count = MARKDOWN_CORE_DELIMITER_MAX_RECORDS;
    engine.capacity = MARKDOWN_CORE_DELIMITER_MAX_RECORDS;
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &ordinary,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            3,
            4,
            0
        ) == MARKDOWN_CORE_DELIMITER_OOM,
        "a push past the id space did not refuse"
    );
    engine.count = count_saved;
    engine.capacity = capacity_saved;
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "the cap probe changed engine state");

    /* An inactive record under a mark's tail: the mark must be refused
     * before any mutation reaches the hook. */
    r3->active = 0;
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, full) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a mark on an inactive tail was accepted"
    );
    r3->active = 1;

    /* The report itself: corrupt a flag the truncation never reads, let
     * the post-mutation check find it, and require the violation to be
     * reported at the mutating site. */
    r1->can_open = 0;
    DR_REQUIRE(
        markdown_core_delimiter_engine_truncate(&engine, full) == MARKDOWN_CORE_DELIMITER_OK,
        "a benign-to-execution corruption blocked truncate"
    );
    DR_REQUIRE(
        dr_invariant_failures == 1 && dr_invariant_site && strcmp(dr_invariant_site, "truncate") == 0,
        "the invariant break was not reported at its mutation site"
    );
    r1->can_open = 1;
    dr_invariant_failures = 0;
    dr_invariant_site = NULL;
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "the report probe left invalid topology");

#undef DR_CORRUPTION

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_randomized_operation_soak(void) {
    static const char case_name[] = "randomized_operation_soak";
    static const markdown_core_delimiter_rule range_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule run_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RUN,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule probe_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        ']',
        dr_close_probe,
    };
    static const uint32_t seeds[] = {0x5EED0001u, 0xC0FFEE42u};
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding bindings[3];
    markdown_core_delimiter_mark marks[16];
    markdown_core_bufsize position = 0;
    uint64_t claim = 0;
    size_t seed_index;
    int result = 0;

    dr_allocator_init(&allocator);
    dr_active_log = NULL;
    bindings[0] = dr_binding(&range_rule, 0);
    bindings[1] = dr_binding(&run_rule, 1);
    bindings[2] = dr_binding(&probe_rule, 2);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 3), "engine start failed");

    for (seed_index = 0; seed_index < sizeof(seeds) / sizeof(seeds[0]); seed_index++) {
        uint32_t state = seeds[seed_index];
        size_t depth = 0;
        size_t op;
        for (op = 0; op < 2000; op++) {
            uint32_t roll = dr_xorshift(&state) % 100;
            if (dr_invariant_failures) {
                fprintf(
                    stderr,
                    "%s: invariant tripped after %s (seed %zu op %zu count %zu)\n",
                    case_name,
                    dr_invariant_site,
                    seed_index,
                    op,
                    engine.count
                );
            }
            DR_REQUIRE(dr_invariant_failures == 0, "the soak tripped an engine invariant mid-run");
            if (roll < 55 && engine.count <= 3000) {
                size_t lane = dr_xorshift(&state) % 3;
                markdown_core_bufsize length = 1 + (markdown_core_bufsize)(dr_xorshift(&state) % 3);
                markdown_core_delimiter_result pushed;
                if (lane == 2) {
                    int opens = dr_xorshift(&state) & 1;
                    pushed = markdown_core_delimiter_engine_push(
                        &engine,
                        &bindings[2],
                        opens,
                        !opens,
                        (markdown_core_node *)&dr_marker_storage,
                        position,
                        position + length,
                        opens ? ++claim : 0
                    );
                } else {
                    uint32_t flags = dr_xorshift(&state) % 3;
                    pushed = markdown_core_delimiter_engine_push(
                        &engine,
                        &bindings[lane],
                        flags != 1,
                        flags != 0,
                        (markdown_core_node *)&dr_marker_storage,
                        position,
                        position + length,
                        0
                    );
                }
                DR_REQUIRE(pushed == MARKDOWN_CORE_DELIMITER_OK, "soak push failed");
                position += length;
            } else if (roll < 70 && depth < 16) {
                marks[depth++] = markdown_core_delimiter_engine_mark(&engine);
            } else if (roll < 85) {
                size_t index = depth ? dr_xorshift(&state) % depth : 0;
                markdown_core_delimiter_mark mark = depth ? marks[index] : (markdown_core_delimiter_mark){0, 0};
                DR_REQUIRE(
                    markdown_core_delimiter_engine_process(&engine, NULL, NULL, mark) == MARKDOWN_CORE_DELIMITER_OK,
                    "soak process failed"
                );
                DR_REQUIRE(engine.count == mark.count, "soak process did not restore its mark");
                depth = index;
            } else {
                markdown_core_delimiter_mark before = markdown_core_delimiter_engine_mark(&engine);
                size_t index = depth ? dr_xorshift(&state) % depth : 0;
                markdown_core_delimiter_mark mark = depth ? marks[index] : (markdown_core_delimiter_mark){0, 0};
                DR_REQUIRE(
                    markdown_core_delimiter_engine_truncate(&engine, mark) == MARKDOWN_CORE_DELIMITER_OK,
                    "soak truncate failed"
                );
                depth = index;
                if (before.count > mark.count) {
                    DR_REQUIRE(
                        markdown_core_delimiter_engine_truncate(&engine, before) == MARKDOWN_CORE_DELIMITER_INVALID,
                        "a mark survived the truncation that reclaimed its tail"
                    );
                }
            }
        }
        DR_REQUIRE(
            markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0}) ==
                MARKDOWN_CORE_DELIMITER_OK,
            "soak drain failed"
        );
        DR_REQUIRE(
            engine.count == 0 && engine.tail == 0 && markdown_core_delimiter_engine_validate(&engine),
            "soak drain left records behind"
        );
        DR_REQUIRE(dr_invariant_failures == 0, "the soak tripped an engine invariant");
    }

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_residual_run_progress(void) {
    static const char case_name[] = "residual_run_progress";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RUN,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    dr_reduction_log log;
    const markdown_core_delimiter_diagnostics *diagnostics;
    uint64_t expected_reductions = (uint64_t)DR_RUN_LENGTH / 2u;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, DR_RUN_LENGTH), "run opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, DR_RUN_LENGTH, DR_RUN_LENGTH), "run closer push failed");

    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "run process left invalid topology");
    DR_REQUIRE(
        log.calls == expected_reductions && diagnostics->reductions == expected_reductions,
        "residual run did not make monotonic two-byte progress"
    );
    DR_REQUIRE(
        diagnostics->opener_candidate_visits == expected_reductions,
        "residual closer performed extra opener searches"
    );
    DR_REQUIRE(
        diagnostics->run_bytes_consumed == 2u * (uint64_t)DR_RUN_LENGTH,
        "residual endpoint accounting did not consume each byte once"
    );
    DR_REQUIRE(diagnostics->unlinks == 2, "residual endpoints were unlinked more than once");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    return result;
}

static int case_geometric_arena_growth(void) {
    static const char case_name[] = "geometric_arena_growth";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    const size_t record_count = 65536;
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    const markdown_core_delimiter_diagnostics *diagnostics;
    size_t expected_capacity = 16;
    uint64_t expected_growths = 1;
    size_t i;
    int result = 0;

    while (expected_capacity < record_count) {
        expected_capacity *= 2;
        expected_growths++;
    }
    dr_allocator_init(&allocator);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    for (i = 0; i < record_count; i++) {
        DR_REQUIRE(dr_push(&engine, &binding, 1, 0, (markdown_core_bufsize)i, 1), "arena push failed");
    }
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "grown arena topology is invalid");
    DR_REQUIRE(
        engine.capacity == expected_capacity && diagnostics->capacity_growths == expected_growths,
        "arena did not grow by one coherent geometric policy"
    );
    DR_REQUIRE(
        allocator.calloc_calls == 1 && allocator.realloc_calls == expected_growths,
        "delimiter records incurred per-record allocation"
    );
    DR_REQUIRE(diagnostics->peak_live_records == record_count, "peak live-record accounting changed");

    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        diagnostics->truncate_visits == record_count && diagnostics->reclaimed_records == record_count,
        "grown arena was not reclaimed in one reverse pass"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_arena_growth_oom_transaction(void) {
    static const char case_name[] = "arena_growth_oom_transaction";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    markdown_core_delimiter_mark before_failure;
    const markdown_core_delimiter_diagnostics *diagnostics;
    size_t i;
    int result = 0;

    dr_allocator_init(&allocator);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    for (i = 0; i < 16; i++) {
        DR_REQUIRE(dr_push(&engine, &binding, 1, 0, (markdown_core_bufsize)i, 1), "initial-capacity push failed");
    }
    before_failure = markdown_core_delimiter_engine_mark(&engine);
    allocator.fail_at = allocator.allocation_attempts + 1;
    DR_REQUIRE(!dr_push(&engine, &binding, 1, 0, 16, 1), "forced arena-growth failure unexpectedly succeeded");
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "failed growth corrupted topology");
    DR_REQUIRE(
        engine.count == before_failure.count && engine.tail == before_failure.tail &&
            diagnostics->pushes == before_failure.count && diagnostics->capacity_growths == 1,
        "failed growth partially published a delimiter"
    );

    allocator.fail_at = 0;
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 16, 1), "growth retry failed");
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "growth retry corrupted topology");
    DR_REQUIRE(
        engine.count == 17 && diagnostics->pushes == 17 && diagnostics->capacity_growths == 2,
        "growth retry did not publish exactly one delimiter"
    );

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_unit_lane_growth_and_reuse(void) {
    static const char case_name[] = "unit_lane_growth_and_reuse";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding first = dr_binding(&rule, 0);
    markdown_core_delimiter_binding added = dr_binding(&rule, 1);
    uint64_t allocations_after_first_unit;
    int result = 0;

    dr_allocator_init(&allocator);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &first, 1, 0, 0, 1), "first-unit push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_capacity == 1 && engine.capacity == 16,
        "first unit did not leave reusable storage"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 2, NULL) == MARKDOWN_CORE_DELIMITER_INVALID,
        "a unit without a concrete capture must be refused"
    );
    allocator.fail_at = allocator.allocation_attempts + 1;
    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 2, &dr_capture) == MARKDOWN_CORE_DELIMITER_OOM,
        "forced lane-growth failure did not fail begin"
    );
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_count == 1 && engine.lane_capacity == 1 &&
            engine.capacity == 16,
        "failed begin discarded storage or changed the lane table"
    );

    allocator.fail_at = 0;
    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 2, &dr_capture) == MARKDOWN_CORE_DELIMITER_OK,
        "second unit did not accept the expanded rule set"
    );
    DR_REQUIRE(dr_push(&engine, &added, 1, 0, 1, 1), "expanded-lane push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_capacity == 2 && engine.capacity == 16,
        "expanded unit did not retain the grown lane table"
    );

    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 1, &dr_capture) == MARKDOWN_CORE_DELIMITER_OK,
        "third unit did not begin"
    );
    allocations_after_first_unit = allocator.allocation_attempts;
    DR_REQUIRE(dr_push(&engine, &first, 1, 0, 2, 1), "third-unit push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        allocator.allocation_attempts == allocations_after_first_unit,
        "retained lane/record capacity allocated again"
    );
    engine.lanes[1].floor_epoch = 1;
    engine.lanes[1].floor[0] = engine.lanes[1].floor[1] = engine.lanes[1].floor[2] = 7;
    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 2, &dr_capture) == MARKDOWN_CORE_DELIMITER_OK,
        "regrown rule set did not begin"
    );
    DR_REQUIRE(
        engine.lanes[1].floor_epoch == 0 && engine.lanes[1].floor[0] == 0 && engine.lanes[1].floor[1] == 0 &&
            engine.lanes[1].floor[2] == 0,
        "reactivated lane retained stale search floors"
    );
    DR_REQUIRE(dr_push(&engine, &added, 1, 0, 3, 1), "reactivated-lane push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        allocator.allocation_attempts == allocations_after_first_unit,
        "reactivated retained lane allocated again"
    );
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "cross-unit reuse left invalid topology");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

static int case_reducer_failure_is_terminal(void) {
    static const char case_name[] = "reducer_failure_is_terminal";
    static const markdown_core_delimiter_rule rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding binding = dr_binding(&rule, 0);
    dr_reduction_log log;
    const markdown_core_delimiter_diagnostics *diagnostics;
    markdown_core_delimiter_result process_result;
    int result = 0;

    dr_allocator_init(&allocator);
    memset(&log, 0, sizeof(log));
    dr_active_log = &log;
    dr_reduction_result = MARKDOWN_CORE_DELIMITER_OOM;
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 1), "engine start failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "first opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 1, 1), "first closer push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 2, 1), "second opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 3, 1), "second closer push failed");

    process_result = markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(process_result == MARKDOWN_CORE_DELIMITER_OOM, "reducer failure was not propagated");
    DR_REQUIRE(log.calls == 1, "engine invoked another reducer after a terminal failure");
    DR_REQUIRE(diagnostics->reductions == 0, "failed callback was counted as a completed reduction");
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && diagnostics->reclaimed_records == 4,
        "terminal failure did not restore the arena mark"
    );
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "failure rollback left invalid topology");

    dr_reduction_result = MARKDOWN_CORE_DELIMITER_INVALID;
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "invalid-result opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 1, 1), "invalid-result closer push failed");
    process_result = markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(process_result == MARKDOWN_CORE_DELIMITER_INVALID, "invalid reducer result was not propagated");
    DR_REQUIRE(
        log.calls == 2 && engine.count == 0 && engine.tail == 0,
        "engine continued after invalid reducer result"
    );

    dr_reduction_result = (markdown_core_delimiter_result)99;
    DR_REQUIRE(dr_push(&engine, &binding, 1, 0, 0, 1), "unknown-result opener push failed");
    DR_REQUIRE(dr_push(&engine, &binding, 0, 1, 1, 1), "unknown-result closer push failed");
    process_result = markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    diagnostics = markdown_core_delimiter_engine_diagnostics(&engine);
    DR_REQUIRE(process_result == MARKDOWN_CORE_DELIMITER_INVALID, "unknown reducer result was not normalized");
    DR_REQUIRE(log.calls == 3, "engine retried after an unknown reducer result");
    DR_REQUIRE(
        diagnostics->reductions == 0 && diagnostics->reclaimed_records == 8 && engine.count == 0 && engine.tail == 0,
        "terminal reducer failures did not restore every arena suffix"
    );
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "terminal result coverage left invalid topology");

cleanup:
    markdown_core_delimiter_engine_free(&engine);
    dr_active_log = NULL;
    dr_reduction_result = MARKDOWN_CORE_DELIMITER_OK;
    return result;
}

static int case_invalid_push_is_transactional(void) {
    static const char case_name[] = "invalid_push_is_transactional";
    static const markdown_core_delimiter_rule ordinary_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule shared_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        ']',
        dr_close_probe,
    };
    static const markdown_core_delimiter_rule negative_pairing_rule = {
        (markdown_core_delimiter_pairing)-1,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule negative_reduction_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        (markdown_core_delimiter_reduction)-1,
        0,
        NULL,
    };
    static const markdown_core_delimiter_rule zero_close_trigger_rule = {
        MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        0,
        dr_close_probe,
    };
    dr_allocator allocator;
    markdown_core_delimiter_engine engine;
    markdown_core_delimiter_binding ordinary = dr_binding(&ordinary_rule, 0);
    markdown_core_delimiter_binding shared = dr_binding(&shared_rule, 1);
    const markdown_core_delimiter_rule *invalid_rules[] = {
        &negative_pairing_rule,
        &negative_reduction_rule,
        &zero_close_trigger_rule,
    };
    const int8_t empty_characters[256] = {0};
    markdown_core_inline_config *config = NULL;
    markdown_core_extension malformed_extension;
    size_t i;
    markdown_core_delimiter_mark mark;
    int result = 0;

    dr_allocator_init(&allocator);
    DR_REQUIRE(dr_engine_start(&engine, &allocator.mem, 2), "engine start failed");
    config = markdown_core_inline_config_new(&allocator.mem, empty_characters, empty_characters);
    DR_REQUIRE(config != NULL, "inline config allocation failed");
    memset(&malformed_extension, 0, sizeof(malformed_extension));
    malformed_extension.insert_inline_from_delim = dr_reduce;
    malformed_extension.delimiter_rule_count = 1;
    for (i = 0; i < sizeof(invalid_rules) / sizeof(invalid_rules[0]); i++) {
        markdown_core_inline_attachment *prepared = (markdown_core_inline_attachment *)&dr_marker_storage;
        malformed_extension.delimiter_rules = invalid_rules[i];
        DR_REQUIRE(
            markdown_core_inline_attachment_prepare(config, &malformed_extension, &prepared) ==
                MARKDOWN_CORE_DELIMITER_INVALID,
            "malformed descriptor was accepted"
        );
        DR_REQUIRE(
            prepared == NULL && config->attachments == NULL && config->extension_rule_count == 0,
            "invalid attachment changed compiled grammar state"
        );
    }
    markdown_core_inline_config_free(config);
    config = NULL;
    DR_REQUIRE(dr_push(&engine, &ordinary, 1, 0, 0, 2), "baseline push failed");
    mark = markdown_core_delimiter_engine_mark(&engine);
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &ordinary,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            1,
            2,
            0
        ) == MARKDOWN_CORE_DELIMITER_INVALID,
        "overlapping source range was accepted"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &shared,
            1,
            1,
            (markdown_core_node *)&dr_marker_storage,
            2,
            3,
            1
        ) == MARKDOWN_CORE_DELIMITER_INVALID,
        "ambiguous shared opener/closer was accepted"
    );
    DR_REQUIRE(
        engine.count == mark.count && engine.tail == mark.tail,
        "invalid push partially published delimiter topology"
    );
    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "invalid push corrupted topology");
    DR_REQUIRE(
        markdown_core_delimiter_engine_push(
            &engine,
            &shared,
            1,
            0,
            (markdown_core_node *)&dr_marker_storage,
            2,
            3,
            7
        ) == MARKDOWN_CORE_DELIMITER_OK,
        "valid shared opener push failed"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_claim_order(
            &engine,
            markdown_core_delimiter_engine_last_open(&engine, &shared)
        ) == 7,
        "shared opener lost its global claim order"
    );

cleanup:
    markdown_core_inline_config_free(config);
    markdown_core_delimiter_engine_free(&engine);
    return result;
}

typedef int (*dr_case_func)(void);

typedef struct {
    const char *name;
    dr_case_func run;
} dr_case;

static const dr_case DR_CASES[] = {
    {"balanced_nearest_ranges", case_balanced_nearest_ranges},
    {"commonmark_modulo_one_floor", case_commonmark_modulo_one_floor},
    {"commonmark_modulo_two_floor", case_commonmark_modulo_two_floor},
    {"commonmark_modulo_zero_pairs", case_commonmark_modulo_zero_pairs},
    {"per_rule_isolation", case_per_rule_isolation},
    {"mark_restore_and_reuse", case_mark_restore_and_reuse},
    {"stale_id_is_refused", case_stale_id_is_refused},
    {"stale_mark_is_refused", case_stale_mark_is_refused},
    {"generation_wrap_is_refused", case_generation_wrap_is_refused},
    {"validator_rejects_corruption", case_validator_rejects_corruption},
    {"randomized_operation_soak", case_randomized_operation_soak},
    {"residual_run_progress", case_residual_run_progress},
    {"geometric_arena_growth", case_geometric_arena_growth},
    {"arena_growth_oom_transaction", case_arena_growth_oom_transaction},
    {"unit_lane_growth_and_reuse", case_unit_lane_growth_and_reuse},
    {"reducer_failure_is_terminal", case_reducer_failure_is_terminal},
    {"invalid_push_is_transactional", case_invalid_push_is_transactional},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    size_t i;
    int list_only = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc) {
            case_name = argv[++i];
        } else {
            fputs("usage: delimiter_engine_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(DR_CASES) / sizeof(DR_CASES[0]); i++) {
            puts(DR_CASES[i].name);
        }
        return 0;
    }
    if (!case_name) {
        fputs("usage: delimiter_engine_runner [--list] [--case NAME]\n", stderr);
        return 2;
    }

    for (i = 0; i < sizeof(DR_CASES) / sizeof(DR_CASES[0]); i++) {
        if (strcmp(DR_CASES[i].name, case_name) == 0) {
            int failed = DR_CASES[i].run();
            markdown_core_concrete_capture_abandon(&dr_capture);
            if (failed == 0 && dr_invariant_failures) {
                fprintf(
                    stderr,
                    "%s: engine invariant failed after %s\n",
                    case_name,
                    dr_invariant_site ? dr_invariant_site : "(unknown site)"
                );
                failed = -1;
            }
            if (failed == 0) {
                printf("%s [PASSED]\n", case_name);
                return 0;
            }
            printf("%s [FAILED]\n", case_name);
            return 1;
        }
    }
    fprintf(stderr, "unknown delimiter-engine case: %s\n", case_name);
    return 2;
}
