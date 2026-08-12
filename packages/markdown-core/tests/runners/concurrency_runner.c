// Native concurrency regression for the read-only markdown_core C facade.
//
// Cases (all exit 0 on success, 1 on any contract violation):
//
//   first_parse  In a fresh process, a barrier releases every thread into its
//                very first markdown_core_document_new simultaneously — no
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
//   documents     Multi-document isolation: a barrier releases every thread
//                into its very first markdown_core_document_new
//                simultaneously; each thread owns one document and streams
//                its own input byte-by-byte with a commit per byte,
//                repeatedly (clear + restream), asserting per-thread dump
//                determinism, monotonically increasing revisions, a stable
//                root id, and a final dump byte-equal to a one-shot parse of
//                the same input — no cross-document interference. A second
//                phase has every thread concurrently traverse and dump one
//                shared document's document between mutating calls, which is
//                exactly the documented read contract.
//
// The runner uses raw native threads (pthread / Win32) on purpose: the
// facade contract must hold without any test-harness serialization.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown_core.h"
#include "commit_compat.h"

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
        while (b->waiting < b->threshold) {
            SleepConditionVariableCS(&b->released, &b->lock, INFINITE);
        }
    }
    LeaveCriticalSection(&b->lock);
}

typedef unsigned(__stdcall *thread_entry)(void *);
#include <process.h>

static int thread_spawn(thread_handle *handle, thread_entry entry, void *argument) {
    uintptr_t raw = _beginthreadex(NULL, 0, entry, argument, 0, NULL);
    if (!raw) {
        return 1;
    }
    *handle = (HANDLE)raw;
    return 0;
}

static int thread_spawn_with_stack(thread_handle *handle, thread_entry entry, void *argument, unsigned int stack_size) {
    uintptr_t raw = _beginthreadex(NULL, stack_size, entry, argument, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!raw) {
        return 1;
    }
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

/* WHAT A CONSUMER DOES. There is no engine-side id->node index: a consumer
 * that holds an id and the tree already has the node, because it meets it on
 * the walk it was doing anyway (requirement 3). These tests hold ids across an
 * edit exactly like a highlighter does, so they find nodes the same way. */
static const markdown_core_node *node_by_id(const markdown_core_node *root, markdown_core_node_id id) {
    const markdown_core_node *node = root;
    if (!root || id == 0) {
        return NULL;
    }
    for (;;) {
        if (markdown_core_node_get_id(node) == id) {
            return node;
        }
        if (markdown_core_node_get_first_child(node)) {
            node = markdown_core_node_get_first_child(node);
            continue;
        }
        while (node != root && !markdown_core_node_get_next_sibling(node)) {
            node = markdown_core_node_get_parent(node);
        }
        if (node == root) {
            return NULL;
        }
        node = markdown_core_node_get_next_sibling(node);
    }
}
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
        while (b->waiting < b->threshold) {
            pthread_cond_wait(&b->released, &b->lock);
        }
    }
    pthread_mutex_unlock(&b->lock);
}

typedef void *(*thread_entry)(void *);

static int thread_spawn(thread_handle *handle, thread_entry entry, void *argument) {
    return pthread_create(handle, NULL, entry, argument) != 0;
}

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN (16 * 1024)
#endif

static int thread_spawn_with_stack(thread_handle *handle, thread_entry entry, void *argument, size_t stack_size) {
    pthread_attr_t attributes;
    int failed;
    if (pthread_attr_init(&attributes) != 0) {
        return 1;
    }
    if (stack_size < (size_t)PTHREAD_STACK_MIN) {
        stack_size = (size_t)PTHREAD_STACK_MIN;
    }
    failed = pthread_attr_setstacksize(&attributes, stack_size) != 0 ||
             pthread_create(handle, &attributes, entry, argument) != 0;
    pthread_attr_destroy(&attributes);
    return failed;
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
        options->directives = false;
        options->cross_links = false;
        options->embeds = false;
        break;
    case OPTIONS_SPLIT:
        options->strikethrough = false;
        options->formulas = false;
        break;
    default:
        break;
    }
}

// Depth-first traversal touching kind, scope, child count, and per-kind
// accessors; returns the node count so results can be sanity-compared.
static size_t traverse(const markdown_core_node *node) {
    size_t visited = 0;
    if (!node) {
        return 0;
    }
    visited += 1;

    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_scope scope = markdown_core_node_scope(node);
    if (!markdown_core_node_kind_name(kind)) {
        return 0;
    }
    if (scope.start.line < 0 || scope.end.line < 0) {
        return 0;
    }

    markdown_core_string view;
    markdown_core_node_literal(node, &view);
    int32_t level;
    markdown_core_node_heading_level(node, &level);
    markdown_core_placement_mode mode;
    markdown_core_node_formula_properties(node, &mode, &view);

    size_t children = 0;
    const markdown_core_node *child = markdown_core_node_get_first_child(node);
    for (; child; child = markdown_core_node_get_next_sibling(child)) {
        size_t below = traverse(child);
        if (!below) {
            return 0;
        }
        visited += below;
        children += 1;
    }
    if (children != markdown_core_node_child_count(node)) {
        return 0;
    }
    return visited;
}

// Parses input+variant, verifies traversal and dump determinism, frees the
// document, and hands the caller a malloc'd dump to compare or discard.
static int parse_and_dump(const char *input, option_variant variant, uint8_t **dump_out, size_t *length_out) {
    markdown_core_parse_options options;
    options_for_variant(variant, &options);

    markdown_core_error *error = NULL;
    markdown_core_document *document =
        markdown_core_document_new(mc_sv((const uint8_t *)input, strlen(input)), &options, &error);
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
    for (int index = 0; index < count; index++) {
        for (size_t slot = 0; slot < INPUT_COUNT * OPTION_VARIANT_COUNT; slot++) {
            markdown_core_dump_free(workers[index].dumps[slot]);
        }
    }
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
    for (int index = 0; index < THREAD_COUNT; index++) {
        thread_join(handles[index]);
    }

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
                    fprintf(
                        stderr,
                        "concurrency: thread %d dump diverges (input %zu variant %d)\n",
                        index,
                        input,
                        variant
                    );
                    failures += 1;
                }
            }
            markdown_core_dump_free(reference);
        }
    }

    worker_release(workers, THREAD_COUNT);
    return failures ? 1 : 0;
}

// --- documents case -----------------------------------------------------------

typedef struct document_worker {
    barrier *start;
    int index;
    int iterations;
    int failed;
    const char *input;
    option_variant variant;
    uint8_t *dump;
    size_t length;
} document_worker;

// Clears the document text, then streams `input` byte-by-byte with a commit
// (and discarded delta) per byte. Hands back a determinism-checked dump.
static int document_stream_once(
    markdown_core_document **document_ref,
    const char *input,
    uint8_t **dump_out,
    size_t *length_out
) {
    markdown_core_error *error = NULL;
    size_t length = strlen(input);
    size_t offset;

    // The caller's text, one byte at a time: this test streams, so the text it
    // hands over grows by one byte per edit. It owns that text; the document
    // is handed the whole of it every time.
    for (offset = 0; offset < length; offset++) {
        markdown_core_commit out;
        memset(&out, 0, sizeof(out));
        markdown_core_document *previous = *document_ref;
        if (!markdown_core_document_edit(previous, mc_sv(input, offset + 1), &out, &error)) {
            markdown_core_document_free(previous);
            *document_ref = NULL;
            markdown_core_error_free(error);
            return 1;
        }
        markdown_core_document_free(previous);
        *document_ref = out.document;
        markdown_core_delta_free(out.delta);
    }

    uint8_t *first = NULL;
    size_t first_length = 0;
    uint8_t *second = NULL;
    size_t second_length = 0;
    if (!markdown_core_document_dump((*document_ref), &first, &first_length, &error) ||
        !markdown_core_document_dump((*document_ref), &second, &second_length, &error)) {
        markdown_core_error_free(error);
        markdown_core_dump_free(first);
        return 1;
    }
    int mismatch = first_length != second_length || memcmp(first, second, first_length) != 0;
    markdown_core_dump_free(second);
    if (mismatch) {
        markdown_core_dump_free(first);
        return 1;
    }
    *dump_out = first;
    *length_out = first_length;
    return 0;
}

static THREAD_RETURN document_worker_main(void *argument) {
    document_worker *self = (document_worker *)argument;
    markdown_core_parse_options options;
    options_for_variant(self->variant, &options);

    barrier_wait(self->start);

    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_new(mc_sv("", 0), &options, &error);
    if (!document) {
        markdown_core_error_free(error);
        self->failed = 1;
        return THREAD_RESULT;
    }

    uint64_t root_id = 0;
    uint64_t last_revision = 0;
    for (int iteration = 0; iteration < self->iterations; iteration++) {
        uint8_t *dump = NULL;
        size_t length = 0;
        if (document_stream_once(&document, self->input, &dump, &length)) {
            self->failed = 1;
            break;
        }

        const markdown_core_node *root = markdown_core_document_root(document);
        uint64_t id = markdown_core_node_get_id(root);
        uint64_t revision = markdown_core_document_revision(document);
        if (id == 0 || (root_id != 0 && id != root_id) || revision <= last_revision || !traverse(root)) {
            markdown_core_dump_free(dump);
            self->failed = 1;
            break;
        }
        root_id = id;
        last_revision = revision;

        if (self->dump) {
            // Later restreams must reproduce the first dump byte-for-byte.
            if (self->length != length || memcmp(self->dump, dump, length) != 0) {
                self->failed = 1;
            }
            markdown_core_dump_free(dump);
            if (self->failed) {
                break;
            }
        } else {
            self->dump = dump;
            self->length = length;
        }
    }

    markdown_core_document_free(document);
    return THREAD_RESULT;
}

typedef struct document_reader {
    barrier *start;
    const markdown_core_document *document;
    const markdown_core_document *view;
    const uint8_t *reference;
    size_t reference_length;
    int failed;
} document_reader;

// An id must round-trip under the concurrent read contract: looking up a
// node's own id resolves back to that node. NULL round-trips vacuously.
static int id_round_trips(const markdown_core_document *document, const markdown_core_node *node) {
    return !node || node_by_id(markdown_core_document_root(document), markdown_core_node_get_id(node)) == node;
}

static THREAD_RETURN document_reader_main(void *argument) {
    document_reader *self = (document_reader *)argument;
    barrier_wait(self->start);

    for (int round = 0; round < 50 && !self->failed; round++) {
        markdown_core_error *error = NULL;
        uint8_t *dump = NULL;
        size_t length = 0;
        const markdown_core_node *root = markdown_core_document_root(self->view);
        if (!id_round_trips(self->document, root) ||
            !id_round_trips(self->document, markdown_core_node_get_first_child(root)) || !traverse(root) ||
            !markdown_core_document_dump(self->view, &dump, &length, &error) || length != self->reference_length ||
            memcmp(dump, self->reference, length) != 0) {
            markdown_core_error_free(error);
            self->failed = 1;
        }
        markdown_core_dump_free(dump);
    }
    return THREAD_RESULT;
}

/* SHARED RECEIVER. An edit reads its document and takes nothing from it, so
 * one document can be edited by several threads at once. What every thread
 * must get back is the SAME DIFF -- the same set of changes over the same
 * content -- because the change set is a function of (old content, new
 * content) and of nothing else. The revisions differ, and must: two
 * successors of one predecessor are two lines of descent, and a node each of
 * them changed differently would otherwise carry one (id, revision) with two
 * contents.
 *
 * The receiver is never written here, which is the whole claim. TSan is what
 * checks it. */
typedef struct shared_edit_worker {
    barrier *start;
    const markdown_core_document *base;
    const char *text;
    uint8_t *dump;
    size_t dump_length;
    uint64_t revision;
    size_t diff_count;
    int failed;
} shared_edit_worker;

static THREAD_RETURN shared_edit_worker_main(void *argument) {
    shared_edit_worker *worker = (shared_edit_worker *)argument;
    markdown_core_commit out;
    markdown_core_error *error = NULL;
    const markdown_core_diff *rows = NULL;

    memset(&out, 0, sizeof(out));
    barrier_wait(worker->start);
    if (!markdown_core_document_edit(worker->base, mc_sv(worker->text, strlen(worker->text)), &out, &error)) {
        markdown_core_error_free(error);
        worker->failed = 1;
        return THREAD_RESULT;
    }
    worker->revision = markdown_core_document_revision(out.document);
    worker->diff_count = markdown_core_delta_diffs(out.delta, &rows);
    if (!markdown_core_document_dump(out.document, &worker->dump, &worker->dump_length, NULL)) {
        worker->failed = 1;
    }
    markdown_core_delta_free(out.delta);
    markdown_core_document_free(out.document);
    return THREAD_RESULT;
}

static int case_shared_edit(void) {
    static shared_edit_worker workers[THREAD_COUNT];
    thread_handle handles[THREAD_COUNT];
    barrier start;
    markdown_core_error *error = NULL;
    markdown_core_document *base = NULL;
    static const char before[] = "# Title\n\nAlpha\n\nBravo\n";
    static const char after[] = "# Title\n\nAlpha changed\n\nBravo\n";
    int failures = 0;
    int index;

    base = markdown_core_document_new(mc_sv(before, sizeof(before) - 1), NULL, &error);
    if (!base) {
        fprintf(stderr, "shared_edit: base document failed to open\n");
        markdown_core_error_free(error);
        return 1;
    }
    barrier_init(&start, THREAD_COUNT);
    for (index = 0; index < THREAD_COUNT; index++) {
        memset(&workers[index], 0, sizeof(workers[index]));
        workers[index].start = &start;
        workers[index].base = base;
        workers[index].text = after;
        if (thread_spawn(&handles[index], shared_edit_worker_main, &workers[index])) {
            fprintf(stderr, "shared_edit: failed to spawn thread %d\n", index);
            markdown_core_document_free(base);
            return 1;
        }
    }
    for (index = 0; index < THREAD_COUNT; index++) {
        thread_join(handles[index]);
    }

    for (index = 0; index < THREAD_COUNT; index++) {
        if (workers[index].failed || !workers[index].dump) {
            fprintf(stderr, "shared_edit: thread %d reported a violation\n", index);
            failures += 1;
            continue;
        }
        // Same text in, same tree and same change set out, on every thread.
        if (workers[index].dump_length != workers[0].dump_length ||
            memcmp(workers[index].dump, workers[0].dump, workers[index].dump_length) != 0) {
            fprintf(stderr, "shared_edit: thread %d produced a different tree\n", index);
            failures += 1;
        }
        if (workers[index].diff_count != workers[0].diff_count) {
            fprintf(stderr, "shared_edit: thread %d produced a different change set\n", index);
            failures += 1;
        }
        // And a revision of its own: no two successors may share one.
        for (int other = 0; other < index; other++) {
            if (workers[other].revision == workers[index].revision) {
                fprintf(stderr, "shared_edit: threads %d and %d share a revision\n", other, index);
                failures += 1;
            }
        }
        if (workers[index].revision <= markdown_core_document_revision(base)) {
            fprintf(stderr, "shared_edit: thread %d did not advance the revision\n", index);
            failures += 1;
        }
    }
    // The receiver survived every one of them.
    if (markdown_core_document_root(base) == NULL) {
        fprintf(stderr, "shared_edit: the shared receiver did not survive\n");
        failures += 1;
    }
    for (index = 0; index < THREAD_COUNT; index++) {
        markdown_core_dump_free(workers[index].dump);
    }
    markdown_core_document_free(base);
    return failures;
}

static int case_documents(void) {
    static document_worker workers[THREAD_COUNT];
    thread_handle handles[THREAD_COUNT];
    barrier start;
    int failures = 0;

    // Phase 1: one isolated document per thread, first document_open under
    // contention, byte-streamed commits overlapping across threads.
    barrier_init(&start, THREAD_COUNT);
    for (int index = 0; index < THREAD_COUNT; index++) {
        memset(&workers[index], 0, sizeof(workers[index]));
        workers[index].start = &start;
        workers[index].index = index;
        workers[index].iterations = 3;
        workers[index].input = INPUTS[(size_t)index % INPUT_COUNT];
        workers[index].variant = (option_variant)(index % OPTION_VARIANT_COUNT);
        if (thread_spawn(&handles[index], document_worker_main, &workers[index])) {
            fprintf(stderr, "documents: failed to spawn thread %d\n", index);
            return 1;
        }
    }
    for (int index = 0; index < THREAD_COUNT; index++) {
        thread_join(handles[index]);
    }

    for (int index = 0; index < THREAD_COUNT; index++) {
        if (workers[index].failed || !workers[index].dump) {
            fprintf(stderr, "documents: thread %d reported a violation\n", index);
            failures += 1;
            continue;
        }
        uint8_t *reference = NULL;
        size_t reference_length = 0;
        if (parse_and_dump(workers[index].input, workers[index].variant, &reference, &reference_length)) {
            fprintf(stderr, "documents: reference parse failed for thread %d\n", index);
            failures += 1;
            continue;
        }
        if (workers[index].length != reference_length ||
            memcmp(workers[index].dump, reference, reference_length) != 0) {
            fprintf(stderr, "documents: thread %d streamed dump diverges from one-shot parse\n", index);
            failures += 1;
        }
        markdown_core_dump_free(reference);
    }
    for (int index = 0; index < THREAD_COUNT; index++) {
        markdown_core_dump_free(workers[index].dump);
    }
    if (failures) {
        return 1;
    }

    // Phase 2: concurrent read-only access to a single document's document
    // between mutating calls.
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_new(mc_sv("", 0), NULL, &error);
    if (!document) {
        markdown_core_error_free(error);
        fprintf(stderr, "documents: shared document open failed\n");
        return 1;
    }
    const char *shared_input = INPUTS[0];
    uint8_t *reference = NULL;
    size_t reference_length = 0;
    markdown_core_document_free(document);
    document = markdown_core_document_new(mc_sv(shared_input, strlen(shared_input)), NULL, &error);
    if (!document || !mc_edit(&document, mc_sv(shared_input, strlen(shared_input)), NULL, &error) ||
        !markdown_core_document_dump(document, &reference, &reference_length, &error)) {
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        fprintf(stderr, "documents: shared document setup failed\n");
        return 1;
    }

    static document_reader readers[THREAD_COUNT];
    barrier read_start;
    barrier_init(&read_start, THREAD_COUNT);
    for (int index = 0; index < THREAD_COUNT; index++) {
        memset(&readers[index], 0, sizeof(readers[index]));
        readers[index].start = &read_start;
        readers[index].document = document;
        readers[index].view = document;
        readers[index].reference = reference;
        readers[index].reference_length = reference_length;
        if (thread_spawn(&handles[index], document_reader_main, &readers[index])) {
            fprintf(stderr, "documents: failed to spawn reader %d\n", index);
            markdown_core_dump_free(reference);
            markdown_core_document_free(document);
            return 1;
        }
    }
    for (int index = 0; index < THREAD_COUNT; index++) {
        thread_join(handles[index]);
    }
    for (int index = 0; index < THREAD_COUNT; index++) {
        if (readers[index].failed) {
            fprintf(stderr, "documents: reader %d observed a divergent document\n", index);
            failures += 1;
        }
    }
    markdown_core_dump_free(reference);

    // The document must still be fully mutable after the readers are done.
    if (!failures) {
        uint8_t *dump = NULL;
        size_t length = 0;
        markdown_core_document_free(document);
        document = markdown_core_document_new(mc_sv("tail\n\n", 6), NULL, &error);
        if (!document || !mc_edit(&document, mc_sv("tail\n\n", 6), NULL, &error) ||
            !markdown_core_document_dump(document, &dump, &length, &error)) {
            markdown_core_error_free(error);
            fprintf(stderr, "documents: post-read commit failed\n");
            failures += 1;
        }
        markdown_core_dump_free(dump);
    }

    markdown_core_document_free(document);
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
    if (parse_and_dump(INPUTS[0], OPTIONS_DEFAULT, &warm, &warm_length)) {
        return 1;
    }
    markdown_core_dump_free(warm);
    return run_threads_and_verify(STRESS_ITERATIONS);
}

static int case_lifecycle(void) {
    uint8_t *first = NULL;
    size_t first_length = 0;
    if (parse_and_dump(INPUTS[1], OPTIONS_DEFAULT, &first, &first_length)) {
        return 1;
    }

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
        if (markdown_core_document_new(mc_sv(NULL, 1), NULL, &error) != NULL ||
            markdown_core_error_get_code(error) != MARKDOWN_CORE_ERROR_INVALID_ARGUMENT) {
            failed = 1;
        }
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
    if (failed) {
        fprintf(stderr, "concurrency: lifecycle regression failed\n");
    }
    return failed;
}

#define SMALL_STACK_QUOTE_DEPTH 4096

typedef struct small_stack_context {
    int failed;
    markdown_core_document *document;
    char *input;
} small_stack_context;

// Worker for dump_small_stack: parse and dump a quote chain whose depth
// would need several times this thread's stack if either path recursed.
// The parse and the dump run here; ownership returns to the spawning
// thread, which frees outside the constrained stack.
static THREAD_RETURN dump_small_stack_worker(void *user) {
    small_stack_context *context = (small_stack_context *)user;
    size_t input_length = (size_t)SMALL_STACK_QUOTE_DEPTH * 2 + 2;
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    context->failed = 1;
    context->input = (char *)malloc(input_length + 1);
    if (!context->input) {
        return THREAD_RESULT;
    }
    for (size_t level = 0; level < (size_t)SMALL_STACK_QUOTE_DEPTH; level++) {
        context->input[level * 2] = '>';
        context->input[level * 2 + 1] = ' ';
    }
    context->input[input_length - 2] = 'a';
    context->input[input_length - 1] = '\n';
    context->input[input_length] = '\0';
    markdown_core_parse_options_init(&options);
    context->document =
        markdown_core_document_new(mc_sv((const uint8_t *)context->input, input_length), &options, &error);
    if (context->document && !error) {
        uint8_t *dump = NULL;
        size_t dump_length = 0;
        if (markdown_core_document_dump(context->document, &dump, &dump_length, &error) && !error) {
            // Every level contributes one line whose prefix grows with
            // depth; a truncated dump would be far smaller.
            context->failed = dump_length < (size_t)SMALL_STACK_QUOTE_DEPTH * 4;
            markdown_core_dump_free(dump);
        }
    }
    markdown_core_error_free(error);
    return THREAD_RESULT;
}

static int case_dump_small_stack(void) {
    // The public parse and canonical dump are iterative, so adversarial
    // nesting must survive a deliberately small thread stack — 256 KiB is
    // far below what recursive descent at this depth would need.
    thread_handle thread;
    small_stack_context context = {1, NULL, NULL};
    if (thread_spawn_with_stack(&thread, dump_small_stack_worker, &context, 256 * 1024)) {
        fprintf(stderr, "concurrency: could not spawn the small-stack thread\n");
        return 1;
    }
    thread_join(thread);
    markdown_core_document_free(context.document);
    free(context.input);
    if (context.failed) {
        fprintf(stderr, "concurrency: deep dump failed on a small thread stack\n");
    }
    return context.failed;
}

int main(int argc, char **argv) {
    const char *case_name = NULL;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
            case_name = argv[++index];
        } else {
            fprintf(
                stderr,
                "usage: concurrency_runner --case first_parse|stress|lifecycle|documents|dump_small_stack\n"
            );
            return 1;
        }
    }
    if (!case_name) {
        fprintf(stderr, "usage: concurrency_runner --case first_parse|stress|lifecycle|documents|dump_small_stack\n");
        return 1;
    }
    if (strcmp(case_name, "first_parse") == 0) {
        return case_first_parse();
    }
    if (strcmp(case_name, "stress") == 0) {
        return case_stress();
    }
    if (strcmp(case_name, "lifecycle") == 0) {
        return case_lifecycle();
    }
    if (strcmp(case_name, "documents") == 0) {
        return case_documents();
    }
    if (strcmp(case_name, "shared_edit") == 0) {
        return case_shared_edit();
    }
    if (strcmp(case_name, "dump_small_stack") == 0) {
        return case_dump_small_stack();
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 1;
}
