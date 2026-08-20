// Native concurrency regression for the read-only markdown_core C facade.
//
// Cases (all exit 0 on success, 1 on any contract violation):
//
//   first_parse  In a fresh process, a barrier releases every thread into its
//                very first markdown_core_document_parse simultaneously — no
//                warmup, no external lock.  Each thread parses, attaches the
//                full extension set via default options, traverses every
//                node, dumps twice, and frees.  All dumps must match the
//                single-threaded reference computed after the threads join.
//
//   stress       After initialization has completed, threads hammer the
//                facade with a matrix of inputs x ParseOptions (extensions
//                toggled on and off) and byte-compare every dump against
//                per-combination references.  This pins the parser-local
//                special-character tables: a parse with an extension
//                disabled must never observe characters registered by a
//                concurrent parse with it enabled.
//
//   lifecycle    Single-threaded registry lifecycle regression: repeated
//                parse/free cycles interleaved with failure paths must keep
//                the process-lifetime registry intact — the last parse must
//                still attach every extension and dump identically to the
//                first.
//
// The runner uses raw native threads (pthread / Win32) on purpose: the
// facade contract must hold without any test-harness serialization.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown_core.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE thread_handle;
typedef struct barrier {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE released;
    int waiting;
    int threshold;
} barrier;

static void barrier_init(barrier *b, int threshold) {
    InitializeCriticalSection(&b->lock);
    InitializeConditionVariable(&b->released);
    b->waiting = 0;
    b->threshold = threshold;
}

static void barrier_wait(barrier *b) {
    EnterCriticalSection(&b->lock);
    b->waiting += 1;
    if (b->waiting >= b->threshold) {
        WakeAllConditionVariable(&b->released);
    } else {
        while (b->waiting < b->threshold)
            SleepConditionVariableCS(&b->released, &b->lock, INFINITE);
    }
    LeaveCriticalSection(&b->lock);
}

typedef unsigned(__stdcall *thread_entry)(void *);
#include <process.h>

static int thread_spawn(thread_handle *handle, thread_entry entry, void *argument) {
    uintptr_t raw = _beginthreadex(NULL, 0, entry, argument, 0, NULL);
    if (!raw)
        return 1;
    *handle = (HANDLE)raw;
    return 0;
}

static void thread_join(thread_handle handle) {
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}

#define THREAD_RETURN unsigned __stdcall
#define THREAD_RESULT 0u
#else
#include <pthread.h>
typedef pthread_t thread_handle;
typedef struct barrier {
    pthread_mutex_t lock;
    pthread_cond_t released;
    int waiting;
    int threshold;
} barrier;

static void barrier_init(barrier *b, int threshold) {
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->released, NULL);
    b->waiting = 0;
    b->threshold = threshold;
}

static void barrier_wait(barrier *b) {
    pthread_mutex_lock(&b->lock);
    b->waiting += 1;
    if (b->waiting >= b->threshold) {
        pthread_cond_broadcast(&b->released);
    } else {
        while (b->waiting < b->threshold)
            pthread_cond_wait(&b->released, &b->lock);
    }
    pthread_mutex_unlock(&b->lock);
}

typedef void *(*thread_entry)(void *);

static int thread_spawn(thread_handle *handle, thread_entry entry, void *argument) {
    return pthread_create(handle, NULL, entry, argument) != 0;
}

static void thread_join(thread_handle handle) { pthread_join(handle, NULL); }

#define THREAD_RETURN void *
#define THREAD_RESULT NULL
#endif

#define THREAD_COUNT 8
#define STRESS_ITERATIONS 200

// Inputs cover every core extension plus emphasis flanking around '~' and
// '$', which is exactly the surface the parser-local skip-character tables
// change when strikethrough/formula are toggled.
static const char *const INPUTS[] = {
    "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
    "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
    "- [x] done\n- [ ] open\n\n> quote with https://example.com autolink\n",
    "Formula $x^2$ inline and *a$b*c$ flanking.\n\n$$\nx = y\n$$\n",
    ":::note[Label]{id=1 .cls title=\"T\"}\ncontent *here*\n:::\n\n"
    "Inline :dir[text]{k=v} tail.\n",
    "Footnote reference[^1] and \"smart\" punctuation -- dashes...\n\n[^1]: note body\n",
};
#define INPUT_COUNT (sizeof(INPUTS) / sizeof(INPUTS[0]))

// Option variants: defaults (everything on), extensions off, and a split set
// so concurrent parsers disagree about '~', '$', and ':'.
typedef enum option_variant {
    OPTIONS_DEFAULT = 0,
    OPTIONS_MINIMAL,
    OPTIONS_SPLIT,
    OPTION_VARIANT_COUNT
} option_variant;

static void options_for_variant(option_variant variant, markdown_core_parse_options *options) {
    markdown_core_parse_options_init(options);
    switch (variant) {
    case OPTIONS_DEFAULT:
        break;
    case OPTIONS_MINIMAL:
        options->smart_punctuation = false;
        options->footnotes = false;
        options->tables = false;
        options->strikethrough = false;
        options->autolinks = false;
        options->task_lists = false;
        options->formulas = false;
        options->dollar_formula_delimiters = false;
        options->latex_formula_delimiters = false;
        options->directives = false;
        break;
    case OPTIONS_SPLIT:
        options->strikethrough = false;
        options->formulas = false;
        options->dollar_formula_delimiters = false;
        options->latex_formula_delimiters = false;
        break;
    default:
        break;
    }
}

// Depth-first traversal touching kind, scope, child count, and per-kind
// accessors; returns the node count so results can be sanity-compared.
static size_t traverse(const markdown_core_node *node) {
    size_t visited = 0;
    if (!node)
        return 0;
    visited += 1;

    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_scope scope = markdown_core_node_scope(node);
    if (!markdown_core_node_kind_name(kind))
        return 0;
    if (scope.start.line < 0 || scope.end.line < 0)
        return 0;

    markdown_core_string_view view;
    markdown_core_node_literal(node, &view);
    int32_t level;
    markdown_core_node_heading_level(node, &level);
    markdown_core_placement_mode mode;
    markdown_core_node_formula_properties(node, &mode, &view);

    size_t children = 0;
    const markdown_core_node *child = markdown_core_node_get_first_child(node);
    for (; child; child = markdown_core_node_get_next_sibling(child)) {
        size_t below = traverse(child);
        if (!below)
            return 0;
        visited += below;
        children += 1;
    }
    if (children != markdown_core_node_child_count(node))
        return 0;
    return visited;
}

// Parses input+variant, verifies traversal and dump determinism, frees the
// document, and hands the caller a malloc'd dump to compare or discard.
static int parse_and_dump(const char *input, option_variant variant, uint8_t **dump_out, size_t *length_out) {
    markdown_core_parse_options options;
    options_for_variant(variant, &options);

    markdown_core_error *error = NULL;
    markdown_core_document *document =
        markdown_core_document_parse((const uint8_t *)input, strlen(input), &options, &error);
    if (!document || error) {
        markdown_core_error_free(error);
        return 1;
    }

    if (!traverse(markdown_core_document_root(document))) {
        markdown_core_document_free(document);
        return 1;
    }

    uint8_t *first = NULL;
    size_t first_length = 0;
    uint8_t *second = NULL;
    size_t second_length = 0;
    if (!markdown_core_document_dump(document, &first, &first_length, &error) ||
        !markdown_core_document_dump(document, &second, &second_length, &error)) {
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        return 1;
    }
    int mismatch = first_length != second_length || memcmp(first, second, first_length) != 0;
    markdown_core_dump_free(second);
    markdown_core_document_free(document);
    if (mismatch) {
        markdown_core_dump_free(first);
        return 1;
    }

    *dump_out = first;
    *length_out = first_length;
    return 0;
}

typedef struct worker {
    barrier *start;
    int index;
    int iterations;
    int failed;
    // One dump per (input, variant) combination produced by this worker.
    uint8_t *dumps[INPUT_COUNT * OPTION_VARIANT_COUNT];
    size_t lengths[INPUT_COUNT * OPTION_VARIANT_COUNT];
} worker;

static THREAD_RETURN worker_main(void *argument) {
    worker *self = (worker *)argument;
    barrier_wait(self->start);

    for (int iteration = 0; iteration < self->iterations; iteration++) {
        for (size_t input = 0; input < INPUT_COUNT; input++) {
            for (int variant = 0; variant < OPTION_VARIANT_COUNT; variant++) {
                // Stagger the starting combination per thread so different
                // option sets genuinely overlap in time.
                size_t combined =
                    (input * OPTION_VARIANT_COUNT + (size_t)variant + (size_t)self->index + (size_t)iteration) %
                    (INPUT_COUNT * OPTION_VARIANT_COUNT);
                size_t real_input = combined / OPTION_VARIANT_COUNT;
                option_variant real_variant = (option_variant)(combined % OPTION_VARIANT_COUNT);

                uint8_t *dump = NULL;
                size_t length = 0;
                if (parse_and_dump(INPUTS[real_input], real_variant, &dump, &length)) {
                    self->failed = 1;
                    return THREAD_RESULT;
                }
                if (self->dumps[combined]) {
                    // Later iterations must reproduce the first byte-for-byte.
                    if (self->lengths[combined] != length || memcmp(self->dumps[combined], dump, length) != 0) {
                        markdown_core_dump_free(dump);
                        self->failed = 1;
                        return THREAD_RESULT;
                    }
                    markdown_core_dump_free(dump);
                } else {
                    self->dumps[combined] = dump;
                    self->lengths[combined] = length;
                }
            }
        }
    }
    return THREAD_RESULT;
}

static void worker_release(worker *workers, int count) {
    for (int index = 0; index < count; index++)
        for (size_t slot = 0; slot < INPUT_COUNT * OPTION_VARIANT_COUNT; slot++)
            markdown_core_dump_free(workers[index].dumps[slot]);
}

// Runs the thread pool, then compares every thread's dump for every
// combination against a reference computed on the main thread (safe once the
// workers have joined: initialization is over).
static int run_threads_and_verify(int iterations) {
    static worker workers[THREAD_COUNT];
    thread_handle handles[THREAD_COUNT];
    barrier start;
    barrier_init(&start, THREAD_COUNT);

    for (int index = 0; index < THREAD_COUNT; index++) {
        memset(&workers[index], 0, sizeof(workers[index]));
        workers[index].start = &start;
        workers[index].index = index;
        workers[index].iterations = iterations;
        if (thread_spawn(&handles[index], worker_main, &workers[index])) {
            fprintf(stderr, "concurrency: failed to spawn thread %d\n", index);
            return 1;
        }
    }
    for (int index = 0; index < THREAD_COUNT; index++)
        thread_join(handles[index]);

    int failures = 0;
    for (int index = 0; index < THREAD_COUNT; index++) {
        if (workers[index].failed) {
            fprintf(stderr, "concurrency: thread %d reported a violation\n", index);
            failures += 1;
        }
    }

    for (size_t input = 0; input < INPUT_COUNT && !failures; input++) {
        for (int variant = 0; variant < OPTION_VARIANT_COUNT; variant++) {
            size_t combined = input * OPTION_VARIANT_COUNT + (size_t)variant;
            uint8_t *reference = NULL;
            size_t reference_length = 0;
            if (parse_and_dump(INPUTS[input], (option_variant)variant, &reference, &reference_length)) {
                fprintf(stderr, "concurrency: reference parse failed (input %zu variant %d)\n", input, variant);
                failures += 1;
                break;
            }
            for (int index = 0; index < THREAD_COUNT; index++) {
                if (!workers[index].dumps[combined]) {
                    fprintf(stderr, "concurrency: thread %d missing dump %zu\n", index, combined);
                    failures += 1;
                    continue;
                }
                if (workers[index].lengths[combined] != reference_length ||
                    memcmp(workers[index].dumps[combined], reference, reference_length) != 0) {
                    fprintf(stderr, "concurrency: thread %d dump diverges (input %zu variant %d)\n", index, input,
                            variant);
                    failures += 1;
                }
            }
            markdown_core_dump_free(reference);
        }
    }

    worker_release(workers, THREAD_COUNT);
    return failures ? 1 : 0;
}

static int case_first_parse(void) {
    // No parse may happen before the barrier releases the workers: the whole
    // point is that library initialization races are exercised for real.
    return run_threads_and_verify(1);
}

static int case_stress(void) {
    // Initialization completes here, single-threaded; the pool then stresses
    // steady-state parsing with disagreeing option sets.
    uint8_t *warm = NULL;
    size_t warm_length = 0;
    if (parse_and_dump(INPUTS[0], OPTIONS_DEFAULT, &warm, &warm_length))
        return 1;
    markdown_core_dump_free(warm);
    return run_threads_and_verify(STRESS_ITERATIONS);
}

static int case_lifecycle(void) {
    uint8_t *first = NULL;
    size_t first_length = 0;
    if (parse_and_dump(INPUTS[1], OPTIONS_DEFAULT, &first, &first_length))
        return 1;

    int failed = 0;
    for (int cycle = 0; cycle < 2000 && !failed; cycle++) {
        size_t input = (size_t)cycle % INPUT_COUNT;
        option_variant variant = (option_variant)(cycle % OPTION_VARIANT_COUNT);
        uint8_t *dump = NULL;
        size_t length = 0;
        if (parse_and_dump(INPUTS[input], variant, &dump, &length)) {
            failed = 1;
            break;
        }
        markdown_core_dump_free(dump);

        // Failure paths must not disturb the registry or later parses.
        markdown_core_error *error = NULL;
        if (markdown_core_document_parse(NULL, 1, NULL, &error) != NULL ||
            markdown_core_error_get_code(error) != MARKDOWN_CORE_ERROR_INVALID_ARGUMENT)
            failed = 1;
        markdown_core_error_free(error);
    }

    if (!failed) {
        uint8_t *last = NULL;
        size_t last_length = 0;
        if (parse_and_dump(INPUTS[1], OPTIONS_DEFAULT, &last, &last_length)) {
            failed = 1;
        } else {
            failed = last_length != first_length || memcmp(last, first, last_length) != 0;
            markdown_core_dump_free(last);
        }
    }
    markdown_core_dump_free(first);
    if (failed)
        fprintf(stderr, "concurrency: lifecycle regression failed\n");
    return failed;
}

int main(int argc, char **argv) {
    const char *case_name = NULL;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
            case_name = argv[++index];
        } else {
            fprintf(stderr, "usage: concurrency_runner --case first_parse|stress|lifecycle\n");
            return 1;
        }
    }
    if (!case_name) {
        fprintf(stderr, "usage: concurrency_runner --case first_parse|stress|lifecycle\n");
        return 1;
    }
    if (strcmp(case_name, "first_parse") == 0)
        return case_first_parse();
    if (strcmp(case_name, "stress") == 0)
        return case_stress();
    if (strcmp(case_name, "lifecycle") == 0)
        return case_lifecycle();
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 1;
}
