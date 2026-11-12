// Native concurrency regression for the read-only markdown_core C facade.
//
// Cases (all exit 0 on success, 1 on any contract violation):
//
//   first_parse  In a fresh process, a barrier releases every thread into its
//                very first markdown_core_document_parse simultaneously — no
//                warmup, no external lock.  Each thread parses the whole
//                dialect through the public entry, traverses every node,
//                dumps twice, and frees.  All dumps must match the
//                single-threaded reference computed after the threads join.
//
//   stress       Threads hammer the parse transaction over a matrix of
//                inputs and byte-compare every dump against a per-input
//                reference.  Every parse attaches the same elements, so
//                this pins instance locality: no parse may observe another's
//                delimiter stack, reference map, or special-character
//                table, and no parse may leave state behind for the next.
//
//   literal      An edit inside a long code block leaves its literal for the
//                first read to join (node.h, A LITERAL, LAZY); a barrier
//                releases every thread into that first read of one
//                document, and each must read the same bytes, the block's
//                lines as the edited text has them.
//
//   lifecycle    Repeated parse/free cycles interleaved with failure paths
//                must not affect a later parser instance — the last parse
//                must still attach every element and dump identically to
//                the first.
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

static void thread_join(thread_handle handle) { pthread_join(handle, NULL); }

#define THREAD_RETURN void *
#define THREAD_RESULT NULL
#endif

#define THREAD_COUNT 8
#define STRESS_ITERATIONS 200

// Inputs cover every core element plus emphasis flanking around '~' and
// '$', which is exactly the surface the parser-local skip-character tables
// change when strikethrough/formula are toggled.
static const char *const INPUTS[] = {
    "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
    "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
    "- [x] done\n- [ ] open\n\n> quote with https://example.com autolink\n",
    "Formula $x^2$ inline and *a$b*c$ flanking.\n\n$$\nx = y\n$$\n",
    ":::note[Label]{id=1 .cls title=\"T\"}\ncontent *here*\n:::\n\n"
    "Inline :dir[text]{k=v} tail.\n",
    "Footnote reference[^1] and \"quoted\" punctuation -- dashes...\n\n[^1]: note body\n",
};
#define INPUT_COUNT (sizeof(INPUTS) / sizeof(INPUTS[0]))

// Every parse goes through the PUBLIC entry, which is the one a consumer
// races: the dialect has no switches, so there is no other language to race
// it against.
static markdown_core_status parse_document(const char *input, markdown_core_document **document) {
    return markdown_core_document_parse((const uint8_t *)input, strlen(input), document);
}

// Depth-first traversal touching kind, id, extent, child count, and per-kind
// accessors; returns the node count so results can be sanity-compared, or 0
// when an accessor refuses a node of the kind it reads.
static size_t traverse(const markdown_core_node *node) {
    size_t visited = 1;

    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_extent extent = markdown_core_node_extent(node);
    const char *name;
    if (markdown_core_node_kind_name(kind, &name) != MARKDOWN_CORE_OK || markdown_core_node_id(node) == 0 ||
        extent.span > UINT32_MAX / 2) {
        return 0;
    }

    markdown_core_string value;
    markdown_core_placement mode;
    int32_t level;
    markdown_core_status status = MARKDOWN_CORE_OK;
    switch (kind) {
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_COMMENT:
        status = markdown_core_node_literal(node, &value);
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        status = markdown_core_node_heading_level(node, &level);
        break;
    case MARKDOWN_CORE_KIND_FORMULA:
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
        status = markdown_core_node_formula_properties(node, &mode, &value);
        break;
    default:
        break;
    }
    if (status != MARKDOWN_CORE_OK) {
        return 0;
    }

    const markdown_core_nodes children = markdown_core_node_children(node);
    for (size_t index = 0; index < children.count; index++) {
        size_t below = traverse(children.items[index]);
        if (!below) {
            return 0;
        }
        visited += below;
    }
    return visited;
}

// Parses one input, verifies traversal and dump determinism, frees the
// document, and hands the caller a malloc'd dump to compare or discard.
static int parse_and_dump(const char *input, uint8_t **dump_out, size_t *length_out) {
    markdown_core_document *document;
    if (parse_document(input, &document) != MARKDOWN_CORE_OK) {
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
    const uint8_t *source = (const uint8_t *)input;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), source, strlen(input), &first,
                                    &first_length) != MARKDOWN_CORE_OK ||
        markdown_core_document_dump(document, markdown_core_document_root(document), source, strlen(input), &second,
                                    &second_length) != MARKDOWN_CORE_OK) {
        markdown_core_dump_free(first);
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
    // One dump per input produced by this worker.
    uint8_t *dumps[INPUT_COUNT];
    size_t lengths[INPUT_COUNT];
} worker;

static THREAD_RETURN worker_main(void *argument) {
    worker *self = (worker *)argument;
    barrier_wait(self->start);

    for (int iteration = 0; iteration < self->iterations; iteration++) {
        for (size_t input = 0; input < INPUT_COUNT; input++) {
            // Stagger the starting input per thread so different inputs
            // genuinely overlap in time.
            size_t combined = (input + (size_t)self->index + (size_t)iteration) % INPUT_COUNT;

            uint8_t *dump = NULL;
            size_t length = 0;
            if (parse_and_dump(INPUTS[combined], &dump, &length)) {
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
    return THREAD_RESULT;
}

static void worker_release(worker *workers, int count) {
    for (int index = 0; index < count; index++) {
        for (size_t slot = 0; slot < INPUT_COUNT; slot++) {
            markdown_core_dump_free(workers[index].dumps[slot]);
        }
    }
}

// Runs the thread pool, then compares every thread's dump for every
// combination against a reference computed on the main thread.
static int run_threads_and_verify(int iterations) {
    worker workers[THREAD_COUNT];
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
        uint8_t *reference = NULL;
        size_t reference_length = 0;
        if (parse_and_dump(INPUTS[input], &reference, &reference_length)) {
            fprintf(stderr, "concurrency: reference parse failed (input %zu)\n", input);
            failures += 1;
            break;
        }
        for (int index = 0; index < THREAD_COUNT; index++) {
            if (!workers[index].dumps[input]) {
                fprintf(stderr, "concurrency: thread %d missing dump %zu\n", index, input);
                failures += 1;
                continue;
            }
            if (workers[index].lengths[input] != reference_length ||
                memcmp(workers[index].dumps[input], reference, reference_length) != 0) {
                fprintf(stderr, "concurrency: thread %d dump diverges (input %zu)\n", index, input);
                failures += 1;
            }
        }
        markdown_core_dump_free(reference);
    }

    worker_release(workers, THREAD_COUNT);
    return failures ? 1 : 0;
}

static int case_first_parse(void) {
    // No parse may happen before the barrier releases the workers: the first
    // calls must be as parallel as steady-state calls without any warmup.
    return run_threads_and_verify(1);
}

static int case_stress(void) {
    // Establish one ordinary sequential baseline, then stress concurrent
    // parsing. There is no library initialization phase for this baseline to
    // perform.
    uint8_t *warm = NULL;
    size_t warm_length = 0;
    if (parse_and_dump(INPUTS[0], &warm, &warm_length)) {
        return 1;
    }
    markdown_core_dump_free(warm);
    return run_threads_and_verify(STRESS_ITERATIONS);
}

#define LITERAL_LINES 400
#define LITERAL_ROUNDS 50

typedef struct reader {
    barrier *start;
    const markdown_core_node *node;
    int code;
    markdown_core_string value;
    markdown_core_status status;
} reader;

static THREAD_RETURN reader_main(void *argument) {
    reader *self = (reader *)argument;
    barrier_wait(self->start);
    if (self->code) {
        markdown_core_optional_string info, language;
        bool fenced, closed;
        self->status =
            markdown_core_node_code_block_properties(self->node, &info, &language, &self->value, &fenced, &closed);
    } else {
        self->status = markdown_core_node_literal(self->node, &self->value);
    }
    return THREAD_RESULT;
}

static int case_literal(void) {
    /* A fenced block and an HTML block, each of LITERAL_LINES lines of 8
     * bytes, `line NNN` and `<i>NNN</i>`... */
    static char text[2 * LITERAL_LINES * 16 + 64];
    size_t size = 0;
    size += (size_t)sprintf(text + size, "```\n");
    for (int i = 0; i < LITERAL_LINES; i++) {
        size += (size_t)sprintf(text + size, "line %03d\n", i);
    }
    size += (size_t)sprintf(text + size, "```\n\n<div>\n");
    for (int i = 0; i < LITERAL_LINES; i++) {
        size += (size_t)sprintf(text + size, "<i>%03d</i>\n", i);
    }
    size += (size_t)sprintf(text + size, "</div>\n");
    markdown_core_session *session;
    const markdown_core_document *document;
    if (markdown_core_session_new((const uint8_t *)text, size, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) !=
            MARKDOWN_CORE_OK ||
        markdown_core_session_flush(session, &document) != MARKDOWN_CORE_OK) {
        fprintf(stderr, "concurrency: literal session failed\n");
        return 1;
    }
    int failed = 0;
    for (int round = 0; round < LITERAL_ROUNDS && !failed; round++) {
        /* One digit of a line in the middle of each block, which the edit's
         * reading leaps in. */
        const char digit = (char)('0' + round % 10);
        const int32_t code_line = 2 + LITERAL_LINES / 2 + round % 7;
        const int32_t html_line = LITERAL_LINES + 5 + LITERAL_LINES / 2 + round % 7;
        const markdown_core_position code_at = {code_line, 8}, code_end = {code_line, 9};
        const markdown_core_position html_at = {html_line, 6}, html_end = {html_line, 7};
        if (markdown_core_session_edit(session, code_at, code_end, (const uint8_t *)&digit, 1, &document) !=
                MARKDOWN_CORE_OK ||
            markdown_core_session_edit(session, html_at, html_end, (const uint8_t *)&digit, 1, &document) !=
                MARKDOWN_CORE_OK) {
            fprintf(stderr, "concurrency: literal edit failed\n");
            failed = 1;
            break;
        }
        const markdown_core_nodes blocks = markdown_core_node_children(markdown_core_document_root(document));
        if (blocks.count != 2) {
            fprintf(stderr, "concurrency: literal blocks %zu\n", blocks.count);
            failed = 1;
            break;
        }
        for (int code = 1; code >= 0 && !failed; code--) {
            /* What the block's lines hold now, from the session's text. */
            const markdown_core_node *node = blocks.items[code ? 0 : 1];
            const size_t from = code ? 4 : 4 + 9 * LITERAL_LINES + 5;
            const size_t length = code ? 9 * LITERAL_LINES : 6 + 11 * LITERAL_LINES + 7;
            uint8_t *bytes = malloc(markdown_core_session_text_size(session));
            if (!bytes) {
                failed = 1;
                break;
            }
            markdown_core_session_text(session, bytes);
            barrier start;
            barrier_init(&start, THREAD_COUNT);
            reader readers[THREAD_COUNT];
            thread_handle threads[THREAD_COUNT];
            for (int i = 0; i < THREAD_COUNT; i++) {
                readers[i] = (reader){&start, node, code, {NULL, 0}, MARKDOWN_CORE_OK};
                if (thread_spawn(&threads[i], reader_main, &readers[i])) {
                    fprintf(stderr, "concurrency: literal spawn failed\n");
                    exit(1);
                }
            }
            for (int i = 0; i < THREAD_COUNT; i++) {
                thread_join(threads[i]);
            }
            for (int i = 0; i < THREAD_COUNT && !failed; i++) {
                if (readers[i].status != MARKDOWN_CORE_OK || readers[i].value.length != length ||
                    readers[i].value.data != readers[0].value.data ||
                    memcmp(readers[i].value.data, bytes + from, length) != 0) {
                    fprintf(stderr, "concurrency: literal round %d block %d reader %d differs\n", round, code, i);
                    failed = 1;
                }
            }
            free(bytes);
        }
    }
    markdown_core_session_close(session);
    return failed;
}

static int case_lifecycle(void) {
    uint8_t *first = NULL;
    size_t first_length = 0;
    if (parse_and_dump(INPUTS[1], &first, &first_length)) {
        return 1;
    }

    int failed = 0;
    for (int cycle = 0; cycle < 2000 && !failed; cycle++) {
        size_t input = (size_t)cycle % INPUT_COUNT;
        uint8_t *dump = NULL;
        size_t length = 0;
        if (parse_and_dump(INPUTS[input], &dump, &length)) {
            failed = 1;
            break;
        }
        markdown_core_dump_free(dump);
    }

    if (!failed) {
        uint8_t *last = NULL;
        size_t last_length = 0;
        if (parse_and_dump(INPUTS[1], &last, &last_length)) {
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
    if (strcmp(case_name, "first_parse") == 0) {
        return case_first_parse();
    }
    if (strcmp(case_name, "stress") == 0) {
        return case_stress();
    }
    if (strcmp(case_name, "literal") == 0) {
        return case_literal();
    }
    if (strcmp(case_name, "lifecycle") == 0) {
        return case_lifecycle();
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 1;
}
