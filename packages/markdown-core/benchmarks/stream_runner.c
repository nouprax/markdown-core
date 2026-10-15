/* The stream benchmark's process: open a subject on an empty text, append one
 * document through it in the chunks of one stream family, report.
 *
 * Nothing here is timed. The driver reads each window's cost out of the
 * callgrind call graph, from the edges into `bench_apply_step`, and the runner
 * marks the windows with `CALLGRIND_DUMP_STATS`, a no-op outside valgrind
 * (docs/plans/2026-09-29-incremental-gates.md, section 5.1). The subject is
 * opened before the first window, so opening is in no window's edge.
 *
 * A stream of at most 1,024 chunks has one chunk per window. A longer stream
 * splits into 1,024 contiguous windows whose chunk counts differ by at most
 * one. The `reparse` subject is measured at each window's last chunk: the
 * chunks before it only extend its text, because reparsing after every chunk
 * of a long stream is the quadratic cost the design removes. The `session`
 * subject takes every chunk in its window.
 *
 *   stream_runner --subject reparse|session --document PATH --stream FAMILY
 *                 --tokens FILE [--final PATH]
 *
 * `--final` writes the subject's final text, whose one-shot parse the driver
 * measures for `oneshot_ir`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <valgrind/callgrind.h>

#include <markdown_core.h>

#include "incremental_harness.h"

#define WINDOWS 1024

/* A subject the runner can measure, and how a chunk that ends no window
 * advances it: NULL when every chunk goes through `bench_apply_step`. */
typedef struct measured_subject {
    const ih_subject_class *subject;
    bool (*advance)(void *subject, const uint8_t *text, size_t length);
} measured_subject;

static const measured_subject SUBJECTS[] = {{&ih_reparse, ih_reparse_extend}, {&ih_session, NULL}};

/* The measured edge: one chunk, from its bytes to the subject's new
 * document. */
__attribute__((noinline)) bool bench_apply_step(const ih_subject_class *subject, void *handle, const ih_step *step,
                                                const markdown_core_document **document) {
    return subject->append(handle, step->text, step->length, document);
}

static int usage(void) {
    fputs("usage: stream_runner --subject NAME --document PATH --stream FAMILY --tokens FILE [--final PATH]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    const char *subject_name = NULL, *document_path = NULL, *family = NULL, *tokens_path = NULL, *final_path = NULL;
    const measured_subject *measured = NULL;
    const markdown_core_document *current = NULL;
    const uint8_t *text;
    uint8_t *document;
    ih_sizes tokens = {0};
    ih_step *steps = NULL;
    size_t *ends = NULL;
    size_t length = 0, count = 0, windows, window, index, final_length = 0;
    void *handle;
    int i, status = 1;

    for (i = 1; i + 1 < argc; i += 2) {
        const char **slot = strcmp(argv[i], "--subject") == 0    ? &subject_name
                            : strcmp(argv[i], "--document") == 0 ? &document_path
                            : strcmp(argv[i], "--stream") == 0   ? &family
                            : strcmp(argv[i], "--tokens") == 0   ? &tokens_path
                            : strcmp(argv[i], "--final") == 0    ? &final_path
                                                                 : NULL;
        if (!slot || *slot) {
            return usage();
        }
        *slot = argv[i + 1];
    }
    if (i != argc || !subject_name || !document_path || !family || !tokens_path) {
        return usage();
    }
    for (index = 0; index < sizeof(SUBJECTS) / sizeof(SUBJECTS[0]); index++) {
        if (strcmp(SUBJECTS[index].subject->name, subject_name) == 0) {
            measured = &SUBJECTS[index];
        }
    }
    if (!measured) {
        fprintf(stderr, "stream_runner: no subject %s\n", subject_name);
        return 2;
    }
    document = ih_read_file(document_path, &length);
    if (!document) {
        fprintf(stderr, "stream_runner: cannot read %s\n", document_path);
        return 1;
    }
    if (!ih_sizes_load(tokens_path, &tokens) || !ih_stream_ends(document, length, family, &tokens, &ends, &count) ||
        !(steps = ih_chunks(document, ends, count))) {
        fprintf(stderr, "stream_runner: cannot chunk %s by %s\n", document_path, family);
        goto done;
    }

    handle = measured->subject->open(IH_UTF8, (const uint8_t *)"", 0, &current);
    if (!handle) {
        fprintf(stderr, "stream_runner: %s did not open\n", subject_name);
        goto done;
    }
    windows = count < WINDOWS ? count : WINDOWS;
    for (window = 0, index = 0; window < windows; window++) {
        size_t end = (window + 1) * count / windows;
        for (; index < end; index++) {
            const markdown_core_document *next = NULL;
            bool appended = index + 1 < end && measured->advance
                                ? measured->advance(handle, steps[index].text, steps[index].length)
                                : bench_apply_step(measured->subject, handle, &steps[index], &next);
            if (!appended) {
                fprintf(stderr, "stream_runner: chunk %zu failed\n", index + 1);
                goto close;
            }
            if (next) {
                current = next;
            }
        }
        CALLGRIND_DUMP_STATS;
    }
    text = measured->subject->text(handle, &final_length);
    if (final_length != length || memcmp(text, document, length) != 0) {
        fputs("stream_runner: the stream did not rebuild its document\n", stderr);
        goto close;
    }
    if (final_path) {
        FILE *file = fopen(final_path, "wb");
        if (!file || fwrite(text, 1, final_length, file) != final_length || fclose(file) != 0) {
            fprintf(stderr, "stream_runner: cannot write %s\n", final_path);
            goto close;
        }
    }
    printf("stream-runner subject=%s steps=%zu windows=%zu bytes=%zu root_children=%zu\n", subject_name, count, windows,
           final_length, markdown_core_node_child_count(markdown_core_document_root(current)));
    status = 0;
close:
    measured->subject->close(handle);
done:
    free(steps);
    free(ends);
    ih_sizes_free(&tokens);
    free(document);
    return status;
}
