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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, lane_count);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);

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
        engine.count == 3 && engine.tail == 3 && markdown_core_delimiter_engine_validate(&engine),
        "forged mark changed engine state"
    );
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, mark);

    DR_REQUIRE(markdown_core_delimiter_engine_validate(&engine), "mark restore left invalid topology");
    DR_REQUIRE(engine.count == 1 && engine.tail == 1, "mark restore did not preserve only the prefix");
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
    DR_REQUIRE(dr_push(&engine, &first, 1, 0, 0, 1), "first-unit push failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_capacity == 1 && engine.capacity == 16,
        "first unit did not leave reusable storage"
    );
    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 2, NULL) == MARKDOWN_CORE_DELIMITER_OK,
        "second unit did not accept the expanded rule set"
    );
    allocator.fail_at = allocator.allocation_attempts + 1;
    DR_REQUIRE(!dr_push(&engine, &added, 1, 0, 1, 1), "forced lane-growth failure unexpectedly succeeded");
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_capacity == 1 && engine.capacity == 16,
        "failed lane growth discarded storage or published a record"
    );

    allocator.fail_at = 0;
    DR_REQUIRE(dr_push(&engine, &added, 1, 0, 1, 1), "lane-growth retry failed");
    markdown_core_delimiter_engine_process(&engine, NULL, NULL, (markdown_core_delimiter_mark){0, 0});
    DR_REQUIRE(
        engine.count == 0 && engine.tail == 0 && engine.lane_capacity == 2 && engine.capacity == 16,
        "expanded unit did not retain the grown lane table"
    );

    DR_REQUIRE(
        markdown_core_delimiter_engine_begin(&engine, 1, NULL) == MARKDOWN_CORE_DELIMITER_OK,
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
        markdown_core_delimiter_engine_begin(&engine, 2, NULL) == MARKDOWN_CORE_DELIMITER_OK,
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 1);
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
    markdown_core_delimiter_engine_init(&engine, &allocator.mem, 2);
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
            if (DR_CASES[i].run() == 0) {
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
