/* The edit and stream benchmark's process: open a subject on a document, run
 * one script or stream through it, report.
 *
 * Nothing here is timed. The driver reads each window's cost out of the
 * callgrind call graph, from the edges into `bench_apply_step`, and the runner
 * marks the windows with `CALLGRIND_DUMP_STATS`, a no-op outside valgrind
 * (docs/plans/2026-09-29-incremental-gates.md, section 5.1). The subject is
 * opened with its document before the first window, so opening is in no
 * window's edge.
 *
 * A script or stream of at most 1,024 steps has one step per window. A longer
 * stream splits into 1,024 contiguous windows whose step counts differ by at
 * most one. The `reparse` subject is measured at each window's last step: the
 * steps before it only extend its text, because reparsing after every chunk of
 * a long stream is the quadratic cost the design removes. The `session`
 * subject takes every step in its window.
 *
 *   edit_runner --subject reparse|session --document PATH --script FILE
 *               --name NAME [--final PATH]
 *   edit_runner --subject reparse|session --document PATH --stream FAMILY
 *               --tokens FILE [--final PATH]
 *
 * `--final` writes the subject's final text, whose one-shot parse the driver
 * measures for `oneshot_ir`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <valgrind/callgrind.h>

#include <markdown_core.h>

#include "edit_harness.h"

#define WINDOWS 1024

/* A subject the runner can measure, and how a step that ends no window
 * advances it: NULL when every step goes through `bench_apply_step`. */
typedef struct measured_subject {
    const eh_subject_class *subject;
    eh_status (*advance)(void *subject, const eh_step *step);
} measured_subject;

static eh_status reparse_advance(void *subject, const eh_step *step) {
    return step->kind == EH_STEP_APPEND ? eh_reparse_apply_append(subject, step->edits[0].text, step->edits[0].length)
                                        : eh_reparse_apply_edit(subject, step->edits, step->count);
}

static const measured_subject SUBJECTS[] = {{&eh_reparse, reparse_advance}, {&eh_session, NULL}};

/* The measured edge: one step, from its arguments to the subject's new
 * document. */
__attribute__((noinline)) eh_status bench_apply_step(const eh_subject_class *subject, void *handle, const eh_step *step,
                                                     const markdown_core_document **document) {
    return step->kind == EH_STEP_APPEND ? subject->append(handle, step->edits[0].text, step->edits[0].length, document)
                                        : subject->edit(handle, step->edits, step->count, document);
}

static int usage(void) {
    fputs("usage: edit_runner --subject NAME --document PATH (--script FILE --name NAME | --stream FAMILY --tokens "
          "FILE) [--final PATH]\n",
          stderr);
    return 2;
}

/* A stream as append steps over the document's bytes; `*chunks` holds their
 * arguments. */
static eh_step *stream_steps(const uint8_t *document, size_t length, const char *family, const char *tokens_path,
                             eh_edit **chunks_out, size_t *count) {
    eh_sizes tokens = {0};
    size_t *ends = NULL;
    eh_step *steps;
    eh_edit *chunks;
    size_t index, from = 0;
    if ((tokens_path && !eh_sizes_load(tokens_path, &tokens)) ||
        !eh_stream_ends(document, length, family, &tokens, &ends, count)) {
        eh_sizes_free(&tokens);
        return NULL;
    }
    eh_sizes_free(&tokens);
    steps = (eh_step *)calloc(*count ? *count : 1, sizeof(*steps));
    chunks = (eh_edit *)calloc(*count ? *count : 1, sizeof(*chunks));
    if (!steps || !chunks) {
        free(steps);
        free(chunks);
        free(ends);
        return NULL;
    }
    for (index = 0; index < *count; index++) {
        chunks[index].text = (uint8_t *)document + from;
        chunks[index].length = ends[index] - from;
        steps[index].kind = EH_STEP_APPEND;
        steps[index].edits = &chunks[index];
        steps[index].count = 1;
        from = ends[index];
    }
    free(ends);
    *chunks_out = chunks;
    return steps;
}

int main(int argc, char **argv) {
    const char *subject_name = NULL, *document_path = NULL, *script_path = NULL, *script_name = NULL;
    const char *family = NULL, *tokens_path = NULL, *final_path = NULL;
    const measured_subject *measured = NULL;
    const eh_script *script = NULL;
    eh_scripts scripts = {0};
    eh_step *stream = NULL;
    eh_edit *chunks = NULL;
    const eh_step *steps;
    const markdown_core_document *current = NULL;
    const uint8_t *text;
    uint8_t *document;
    size_t length = 0, count = 0, windows, window, index, final_length = 0;
    void *handle;
    int i, status = 1;

    for (i = 1; i + 1 < argc; i += 2) {
        const char **slot = strcmp(argv[i], "--subject") == 0    ? &subject_name
                            : strcmp(argv[i], "--document") == 0 ? &document_path
                            : strcmp(argv[i], "--script") == 0   ? &script_path
                            : strcmp(argv[i], "--name") == 0     ? &script_name
                            : strcmp(argv[i], "--stream") == 0   ? &family
                            : strcmp(argv[i], "--tokens") == 0   ? &tokens_path
                            : strcmp(argv[i], "--final") == 0    ? &final_path
                                                                 : NULL;
        if (!slot || *slot) {
            return usage();
        }
        *slot = argv[i + 1];
    }
    if (i != argc || !subject_name || !document_path || !script_path == !family || !script_path != !script_name) {
        return usage();
    }
    for (index = 0; index < sizeof(SUBJECTS) / sizeof(SUBJECTS[0]); index++) {
        if (strcmp(SUBJECTS[index].subject->name, subject_name) == 0) {
            measured = &SUBJECTS[index];
        }
    }
    if (!measured) {
        fprintf(stderr, "edit_runner: no subject %s\n", subject_name);
        return 2;
    }
    document = eh_read_file(document_path, &length);
    if (!document) {
        fprintf(stderr, "edit_runner: cannot read %s\n", document_path);
        return 1;
    }
    if (script_path) {
        if (!eh_scripts_load(script_path, &scripts)) {
            free(document);
            return 1;
        }
        for (index = 0; index < scripts.count; index++) {
            if (strcmp(scripts.scripts[index].name, script_name) == 0) {
                script = &scripts.scripts[index];
            }
        }
        if (!script) {
            fprintf(stderr, "edit_runner: %s holds no script %s\n", script_path, script_name);
            eh_scripts_free(&scripts);
            free(document);
            return 1;
        }
        steps = script->steps;
        count = script->count;
    } else {
        stream = stream_steps(document, length, family, tokens_path, &chunks, &count);
        if (!stream) {
            fprintf(stderr, "edit_runner: cannot chunk %s by %s\n", document_path, family);
            free(document);
            return 1;
        }
        steps = stream;
    }
    for (index = 0; index < count; index++) {
        if (steps[index].kind != EH_STEP_EDIT && steps[index].kind != EH_STEP_APPEND) {
            fprintf(stderr, "edit_runner: step %zu is not a valid step\n", index + 1);
            goto done;
        }
    }

    handle = measured->subject->open(EH_UTF8, stream ? (const uint8_t *)"" : document, stream ? 0 : length, &current);
    if (!handle) {
        fprintf(stderr, "edit_runner: %s did not open\n", subject_name);
        goto done;
    }
    windows = count < WINDOWS ? count : WINDOWS;
    for (window = 0, index = 0; window < windows; window++) {
        size_t end = (window + 1) * count / windows;
        for (; index < end; index++) {
            const markdown_core_document *next = NULL;
            eh_status step = index + 1 < end && measured->advance
                                 ? measured->advance(handle, &steps[index])
                                 : bench_apply_step(measured->subject, handle, &steps[index], &next);
            if (step != EH_OK) {
                fprintf(stderr, "edit_runner: step %zu failed\n", index + 1);
                goto close;
            }
            if (next) {
                current = next;
            }
        }
        CALLGRIND_DUMP_STATS;
    }
    text = measured->subject->text(handle, &final_length);
    if (stream && (final_length != length || memcmp(text, document, length) != 0)) {
        fputs("edit_runner: the stream did not rebuild its document\n", stderr);
        goto close;
    }
    if (final_path) {
        FILE *file = fopen(final_path, "wb");
        if (!file || fwrite(text, 1, final_length, file) != final_length || fclose(file) != 0) {
            fprintf(stderr, "edit_runner: cannot write %s\n", final_path);
            goto close;
        }
    }
    printf("edit-runner subject=%s steps=%zu windows=%zu bytes=%zu root_children=%zu\n", subject_name, count, windows,
           final_length, markdown_core_nodes_count(markdown_core_node_children(markdown_core_document_root(current))));
    status = 0;
close:
    measured->subject->close(handle);
done:
    free(chunks);
    free(stream);
    eh_scripts_free(&scripts);
    free(document);
    return status;
}
