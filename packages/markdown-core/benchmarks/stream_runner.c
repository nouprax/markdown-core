/* The stream benchmark's process: open a subject on an empty text, append one
 * document through it in the chunks of one stream family, report; or open it
 * on a document, flush, run each edit script of one file, report.
 *
 * Nothing here is timed. The driver reads each window's cost out of the
 * callgrind call graph, from the edges into `bench_apply_step` and
 * `bench_flush` or, for `reparse`, the parse stages under them, and the
 * runner marks the windows with `CALLGRIND_DUMP_STATS` and
 * `CALLGRIND_ZERO_STATS`, no-ops outside valgrind
 * (docs/plans/2026-09-29-incremental-gates.md, section 5.1). The subject is
 * opened before the counts are zeroed, so opening is in no window's dump.
 *
 * `--windows` is the most windows the driver splits the stream into (5.1): a
 * stream of at most that many chunks has one chunk per window, and a longer
 * one splits into that many contiguous windows whose chunk counts differ by
 * at most one. The `reparse` subject is measured at each window's last
 * chunk: the chunks before it only extend its text, because reparsing after
 * every chunk of a long stream is the quadratic cost the design removes. The
 * `session` subject takes every chunk in its window. The stream's flush is
 * one more window, after the last chunk's.
 *
 * An edit script splits its edits into windows the same way, each script on
 * its own: the subject opens on the document and flushes before the counts
 * are zeroed, so neither is in a window, and `reparse` replaces the text of
 * every edit but a window's last without parsing it. A script has no
 * flush window. Only a subject that takes edits runs scripts.
 *
 * The `binding` subject is the session stepped through the MCB3 entries, as
 * the bindings that read messages step it: the driver reads what its
 * messages add to the session's steps, the C side of building each published
 * document's values (plan D5).
 *
 *   stream_runner --subject reparse|session|binding --document PATH
 *                 --stream FAMILY --tokens FILE --windows N [--final PATH]
 *   stream_runner --subject NAME --document PATH --scripts FILE --windows N
 *
 * `--final` writes the subject's final text, whose one-shot parse the driver
 * measures for `oneshot_ir`. A script run prints one receipt per script, in
 * the file's order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <valgrind/callgrind.h>

#include <markdown_core.h>

#include "incremental_harness.h"
#include "markdown_core_receipt.h"
#include "markdown_core_wire.h"

/* A subject the runner can measure, and how a chunk or an edit that ends no
 * window advances it: NULL when every one goes through `bench_apply_step`. */
typedef struct measured_subject {
    const ih_subject_class *subject;
    bool (*advance)(void *subject, const uint8_t *text, size_t length);
    markdown_core_status (*replace)(void *subject, size_t start, size_t end, const uint8_t *text, size_t length);
} measured_subject;

/* THE BINDING SUBJECT: the `session` subject's session, each step answering
 * with its document's MCB3 message (plan 6.2), which the binding releases
 * once it has built its values from it. */

/* A message's status byte follows the magic and the message length
 * (docs/architecture/wire-format.md): 0 is a document, and a failure carries
 * its status in the u32 after it. */
enum { MESSAGE_STATUS = 8 };

static markdown_core_status binding_step(void *handle, uint8_t *message, const markdown_core_document **document) {
    const markdown_core_status status =
        !message ? MARKDOWN_CORE_ALLOCATION_FAILED
        : !message[MESSAGE_STATUS]
            ? MARKDOWN_CORE_OK
            : (markdown_core_status)(message[MESSAGE_STATUS + 1] | (uint32_t)message[MESSAGE_STATUS + 2] << 8 |
                                     (uint32_t)message[MESSAGE_STATUS + 3] << 16 |
                                     (uint32_t)message[MESSAGE_STATUS + 4] << 24);
    markdown_core_wire_free(message);
    *document = status == MARKDOWN_CORE_OK ? markdown_core_session_document(ih_session_of(handle)) : NULL;
    return status;
}

static void *binding_open(ih_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    return ih_session.open(unit, text, length, document);
}

static markdown_core_status binding_append(void *handle, const uint8_t *text, size_t length,
                                           const markdown_core_document **document) {
    return binding_step(handle, markdown_core_wire_session_append(ih_session_of(handle), text, length), document);
}

static markdown_core_status binding_flush(void *handle, const markdown_core_document **document) {
    return binding_step(handle, markdown_core_wire_session_flush(ih_session_of(handle)), document);
}

static const uint8_t *binding_text(const void *handle, size_t *length) { return ih_session.text(handle, length); }

static void binding_close(void *handle) { ih_session.close(handle); }

static const ih_subject_class binding = {.name = "binding",
                                         .open = binding_open,
                                         .append = binding_append,
                                         .flush = binding_flush,
                                         .text = binding_text,
                                         .close = binding_close};

static const measured_subject SUBJECTS[] = {
    {&ih_reparse, ih_reparse_extend, ih_reparse_replace}, {&ih_session, NULL, NULL}, {&binding, NULL, NULL}};

/* The measured edges: one chunk or edit, from its bytes to the subject's new
 * document, and the flush, to the final document. */
__attribute__((noinline)) bool bench_apply_step(const ih_subject_class *subject, void *handle, const ih_step *step,
                                                const markdown_core_document **document) {
    return (step->kind == IH_EDIT ? subject->edit(handle, step->start, step->end, step->text, step->length, document)
                                  : subject->append(handle, step->text, step->length, document)) == MARKDOWN_CORE_OK;
}

__attribute__((noinline)) bool bench_flush(const ih_subject_class *subject, void *handle,
                                           const markdown_core_document **document) {
    return subject->flush(handle, document) == MARKDOWN_CORE_OK;
}

static int usage(void) {
    fputs("usage: stream_runner --subject NAME --document PATH (--stream FAMILY --tokens FILE --windows N [--final "
          "PATH] | --scripts FILE --windows N)\n",
          stderr);
    return 2;
}

/* One edit script on the document, in at most `most` windows of its edits,
 * then its receipt. The script flushes, then edits: the generator's edit
 * families take no other step. */
static bool run_script(const measured_subject *measured, const uint8_t *document, size_t length,
                       const ih_script *script, size_t most) {
    const markdown_core_document *current = NULL;
    const ih_step *edits = script->steps + 1;
    const size_t count = script->count ? script->count - 1 : 0;
    const uint8_t *text;
    ih_text model = {0};
    size_t windows, window, index, final_length = 0, root_children = 0;
    bool done = false;
    void *handle;

    for (index = 0; index < count; index++) {
        if (edits[index].kind != IH_EDIT) {
            break;
        }
    }
    if (!count || script->steps[0].kind != IH_FLUSH || index != count) {
        fprintf(stderr, "stream_runner: %s does not flush and then edit\n", script->name);
        return false;
    }
    handle = measured->subject->open(IH_UTF8, document, length, &current);
    if (!handle) {
        fprintf(stderr, "stream_runner: %s did not open\n", measured->subject->name);
        return false;
    }
    if (measured->subject->flush(handle, &current) != MARKDOWN_CORE_OK || !ih_text_assign(&model, document, length)) {
        fprintf(stderr, "stream_runner: %s did not flush\n", script->name);
        goto close;
    }
    CALLGRIND_ZERO_STATS;
    windows = count < most ? count : most;
    for (window = 0, index = 0; window < windows; window++) {
        size_t end = (window + 1) * count / windows;
        for (; index < end; index++) {
            const ih_step *step = &edits[index];
            const markdown_core_document *next = NULL;
            bool edited =
                index + 1 < end && measured->replace
                    ? measured->replace(handle, step->start, step->end, step->text, step->length) == MARKDOWN_CORE_OK
                    : bench_apply_step(measured->subject, handle, step, &next);
            if (!edited || !ih_text_replace(&model, step->start, step->end, step->text, step->length)) {
                fprintf(stderr, "stream_runner: %s edit %zu failed\n", script->name, index + 1);
                goto close;
            }
            if (next) {
                current = next;
            }
        }
        CALLGRIND_DUMP_STATS;
    }
    text = measured->subject->text(handle, &final_length);
    if (final_length != model.length || memcmp(text, model.bytes, final_length) != 0) {
        fprintf(stderr, "stream_runner: %s did not end in its edits' text\n", script->name);
        goto close;
    }
    if (!bench_root_children(current, text, final_length, &root_children)) {
        fprintf(stderr, "stream_runner: cannot read the receipt of %s\n", script->name);
        goto close;
    }
    printf("stream-runner subject=%s script=%s edits=%zu windows=%zu bytes=%zu root_children=%zu\n",
           measured->subject->name, script->name, count, windows, final_length, root_children);
    done = true;
close:
    measured->subject->close(handle);
    ih_text_free(&model);
    return done;
}

/* Every script of `path`, each on the document. */
static int run_scripts(const measured_subject *measured, const uint8_t *document, size_t length, const char *path,
                       size_t most) {
    ih_scripts scripts = {0};
    size_t index;
    int status = 0;
    if (!measured->subject->edit) {
        fprintf(stderr, "stream_runner: %s takes no edits\n", measured->subject->name);
        return 2;
    }
    if (!ih_scripts_load(path, &scripts)) {
        return 1;
    }
    for (index = 0; index < scripts.count && !status; index++) {
        status = run_script(measured, document, length, &scripts.scripts[index], most) ? 0 : 1;
    }
    ih_scripts_free(&scripts);
    return status;
}

int main(int argc, char **argv) {
    const char *subject_name = NULL, *document_path = NULL, *family = NULL, *tokens_path = NULL, *final_path = NULL,
               *windows_text = NULL, *scripts_path = NULL;
    const measured_subject *measured = NULL;
    const markdown_core_document *current = NULL;
    const uint8_t *text;
    uint8_t *document;
    ih_sizes tokens = {0};
    ih_step *steps = NULL;
    size_t *ends = NULL;
    size_t length = 0, count = 0, most = 0, windows, window, index, final_length = 0, root_children = 0;
    char *windows_end = NULL;
    void *handle;
    int i, status = 1;

    for (i = 1; i + 1 < argc; i += 2) {
        const char **slot = strcmp(argv[i], "--subject") == 0    ? &subject_name
                            : strcmp(argv[i], "--document") == 0 ? &document_path
                            : strcmp(argv[i], "--stream") == 0   ? &family
                            : strcmp(argv[i], "--tokens") == 0   ? &tokens_path
                            : strcmp(argv[i], "--windows") == 0  ? &windows_text
                            : strcmp(argv[i], "--final") == 0    ? &final_path
                            : strcmp(argv[i], "--scripts") == 0  ? &scripts_path
                                                                 : NULL;
        if (!slot || *slot) {
            return usage();
        }
        *slot = argv[i + 1];
    }
    if (i != argc || !subject_name || !document_path || !windows_text ||
        (scripts_path ? family || tokens_path || final_path : !family || !tokens_path)) {
        return usage();
    }
    most = strtoul(windows_text, &windows_end, 10);
    if (!most || *windows_end) {
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
    if (scripts_path) {
        status = run_scripts(measured, document, length, scripts_path, most);
        free(document);
        return status;
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
    CALLGRIND_ZERO_STATS;
    windows = count < most ? count : most;
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
    if (!bench_flush(measured->subject, handle, &current)) {
        fputs("stream_runner: the flush failed\n", stderr);
        goto close;
    }
    CALLGRIND_DUMP_STATS;
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
    if (!bench_root_children(current, text, final_length, &root_children)) {
        fputs("stream_runner: cannot read the final document's receipt\n", stderr);
        goto close;
    }
    /* The windows of the chunks and the flush's. */
    printf("stream-runner subject=%s steps=%zu windows=%zu bytes=%zu root_children=%zu\n", subject_name, count,
           windows + 1, final_length, root_children);
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
