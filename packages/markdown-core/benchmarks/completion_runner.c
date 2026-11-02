/* The completion latency benchmark's process: open the language engine on a
 * document, then run each edit script of one file through it as a client
 * typing the script would, measuring some of its completion requests
 * (docs/plans/2026-10-23-lsp-completion-engine.md, section 7).
 *
 * Nothing here is timed. A request is the didChange of one edit followed by
 * the completion at the end of the edit's text, the place the client's cursor
 * stands after typing it, both inside `bench_request`. The driver reads each
 * request's cost out of the callgrind call graph from the edges into
 * `bench_request`, and the runner marks the requests with
 * `CALLGRIND_DUMP_STATS` and `CALLGRIND_ZERO_STATS`, no-ops outside valgrind.
 * Creating the server, initialize and didOpen, which parses and flushes the
 * document, come before the counts are zeroed, and turning each edit's byte
 * offsets into protocol positions is the client's work, done before the
 * call, so none of them is in a request.
 *
 * `--requests` is the most requests a script is measured at. A script of at
 * most that many edits is measured at every edit; a longer one splits into
 * that many runs of edits whose lengths differ by at most one, and is
 * measured at each run's last edit, after the edits before it reach the
 * server as changes of their own outside every request. Positions are UTF-8,
 * the unit the engine prefers.
 *
 * `--edits` cuts each script to its first N edits, so a large document's
 * requests do not wait on every edit of a long script under callgrind; the
 * receipt counts the edits the runner sent, and the text check reads their
 * text.
 *
 *   completion_runner --document PATH --scripts FILE --requests N [--edits N]
 *
 * Each script prints one receipt, in the file's order, after the runner has
 * checked that the document's session ends in the script's text, or a
 * receipt that it skipped the script, which makes no request: one whose edit
 * splits a `\r\n`, which no protocol position can address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <valgrind/callgrind.h>

#include <markdown_core.h>
#include <markdown_core_lsp.h>

#include "incremental_harness.h"
#include "text.h"

static const uint8_t URI[] = "file:///bench.md";
enum { URI_SIZE = sizeof(URI) - 1 };

/* The protocol position of byte `offset` of `text`, in UTF-8: the line breaks
 * before it, `\r\n` counting once, and the bytes since its line's start. */
static markdown_core_lsp_position position_of(const ih_text *text, size_t offset) {
    markdown_core_lsp_position position = {0, 0};
    size_t start = 0;
    for (size_t at = 0; at < offset; at++) {
        const uint8_t byte = text->bytes[at];
        if (byte == '\n' || (byte == '\r' && (at + 1 >= text->length || text->bytes[at + 1] != '\n'))) {
            position.line++;
            start = at + 1;
        }
    }
    position.character = (uint32_t)(offset - start);
    return position;
}

/* Whether some edit of the script starts or ends between the `\r` and the
 * `\n` of a line break, a place no protocol position names: a client never
 * sends such an edit, so the runner skips the script. */
static bool splits_break(const uint8_t *document, size_t length, const ih_step *steps, size_t count) {
    ih_text model = {0};
    bool split = false;
    if (!ih_text_assign(&model, document, length)) {
        return true;
    }
    for (size_t index = 0; index < count && !split; index++) {
        const ih_step *step = &steps[index];
        for (int side = 0; side < 2 && !split; side++) {
            const size_t at = side ? step->end : step->start;
            split = at > 0 && at < model.length && model.bytes[at - 1] == '\r' && model.bytes[at] == '\n';
        }
        if (!split && !ih_text_replace(&model, step->start, step->end, step->text, step->length)) {
            split = true;
        }
    }
    ih_text_free(&model);
    return split;
}

/* The measured edge: one request, an edit and the completion at `cursor`,
 * from the client's messages to the server's answer. */
__attribute__((noinline)) bool bench_request(markdown_core_lsp_server *server, int32_t version,
                                             const markdown_core_lsp_edit *edit, markdown_core_lsp_position cursor) {
    markdown_core_lsp_list *list = NULL;
    if (markdown_core_lsp_update(server, URI, URI_SIZE, version, edit, 1) != MARKDOWN_CORE_LSP_OK ||
        markdown_core_lsp_complete(server, URI, URI_SIZE, cursor, &list) != MARKDOWN_CORE_LSP_OK) {
        return false;
    }
    const bool current = markdown_core_lsp_list_version(list) == version;
    markdown_core_lsp_list_free(list);
    return current;
}

static int usage(void) {
    fputs("usage: completion_runner --document PATH --scripts FILE --requests N [--edits N]\n", stderr);
    return 2;
}

/* One edit script on the document, measured at most `most` times, then its
 * receipt. The script flushes, then edits: the generator's edit families take
 * no other step, and didOpen is the flush. */
static bool run_script(const uint8_t *document, size_t length, const ih_script *script, size_t most, size_t cut) {
    const ih_step *steps = script->steps + 1;
    size_t count = script->count ? script->count - 1 : 0;
    const markdown_core_text_unit unit = MARKDOWN_CORE_TEXT_UNIT_UTF8;
    markdown_core_lsp_capabilities capabilities;
    markdown_core_lsp_server *server = NULL;
    ih_text model = {0};
    size_t requests, request, index;
    int32_t version = 1;
    bool done = false;

    for (index = 0; index < count; index++) {
        if (steps[index].kind != IH_EDIT) {
            break;
        }
    }
    if (!count || script->steps[0].kind != IH_FLUSH || index != count) {
        fprintf(stderr, "completion_runner: %s does not flush and then edit\n", script->name);
        return false;
    }
    if (cut && count > cut) {
        count = cut;
    }
    if (splits_break(document, length, steps, count)) {
        printf("completion-runner script=%s skipped\n", script->name);
        return true;
    }
    requests = count < most ? count : most;
    if (markdown_core_lsp_server_new(&server) != MARKDOWN_CORE_LSP_OK || !ih_text_assign(&model, document, length) ||
        markdown_core_lsp_initialize(server, &unit, 1, &capabilities) != MARKDOWN_CORE_LSP_OK ||
        capabilities.unit != MARKDOWN_CORE_TEXT_UNIT_UTF8 ||
        markdown_core_lsp_open(server, URI, URI_SIZE, version, document, length) != MARKDOWN_CORE_LSP_OK) {
        fprintf(stderr, "completion_runner: %s did not open\n", script->name);
        goto close;
    }
    CALLGRIND_ZERO_STATS;
    for (request = 0, index = 0; request < requests; request++) {
        const size_t measured = (request + 1) * count / requests - 1;
        for (; index <= measured; index++) {
            /* The client's half: the edit's range in the text the edits
             * before it made, which the model follows. */
            const ih_step *step = &steps[index];
            const markdown_core_lsp_edit edit = {
                {position_of(&model, step->start), position_of(&model, step->end)}, step->text, step->length};
            if (!ih_text_replace(&model, step->start, step->end, step->text, step->length)) {
                fprintf(stderr, "completion_runner: %s edit %zu does not fit its text\n", script->name, index + 1);
                goto close;
            }
            version++;
            const bool sent =
                index < measured
                    ? markdown_core_lsp_update(server, URI, URI_SIZE, version, &edit, 1) == MARKDOWN_CORE_LSP_OK
                    : bench_request(server, version, &edit, position_of(&model, step->start + step->length));
            if (!sent) {
                fprintf(stderr, "completion_runner: %s edit %zu failed\n", script->name, index + 1);
                goto close;
            }
            if (index < measured) {
                /* Outside every request: the next one's counts start here. */
                CALLGRIND_ZERO_STATS;
            }
        }
        CALLGRIND_DUMP_STATS;
    }
    {
        const markdown_core_string text = markdown_core_session_source(lsp_server_session(server, URI, URI_SIZE));
        if (text.length != model.length || memcmp(text.data, model.bytes, model.length) != 0) {
            fprintf(stderr, "completion_runner: %s did not end in its edits' text\n", script->name);
            goto close;
        }
    }
    printf("completion-runner script=%s edits=%zu requests=%zu bytes=%zu\n", script->name, count, requests,
           model.length);
    done = true;
close:
    markdown_core_lsp_server_free(server);
    ih_text_free(&model);
    return done;
}

int main(int argc, char **argv) {
    const char *document_path = NULL, *scripts_path = NULL, *requests_text = NULL, *edits_text = NULL;
    ih_scripts scripts = {0};
    uint8_t *document;
    size_t length = 0, most, cut = 0, index;
    char *requests_end = NULL, *edits_end = NULL;
    int i, status = 0;

    for (i = 1; i + 1 < argc; i += 2) {
        const char **slot = strcmp(argv[i], "--document") == 0   ? &document_path
                            : strcmp(argv[i], "--scripts") == 0  ? &scripts_path
                            : strcmp(argv[i], "--requests") == 0 ? &requests_text
                            : strcmp(argv[i], "--edits") == 0    ? &edits_text
                                                                 : NULL;
        if (!slot || *slot) {
            return usage();
        }
        *slot = argv[i + 1];
    }
    if (i != argc || !document_path || !scripts_path || !requests_text) {
        return usage();
    }
    most = strtoul(requests_text, &requests_end, 10);
    if (!most || *requests_end) {
        return usage();
    }
    if (edits_text) {
        cut = strtoul(edits_text, &edits_end, 10);
        if (!cut || *edits_end) {
            return usage();
        }
    }
    document = ih_read_file(document_path, &length);
    if (!document) {
        fprintf(stderr, "completion_runner: cannot read %s\n", document_path);
        return 1;
    }
    if (!ih_scripts_load(scripts_path, &scripts)) {
        free(document);
        return 1;
    }
    for (index = 0; index < scripts.count && !status; index++) {
        status = run_script(document, length, &scripts.scripts[index], most, cut) ? 0 : 1;
    }
    ih_scripts_free(&scripts);
    free(document);
    return status;
}
