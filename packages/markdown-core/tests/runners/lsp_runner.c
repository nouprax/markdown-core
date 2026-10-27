/*
 * The language engine's suites (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 8).
 *
 *   lsp_runner --transcripts FILE...
 *
 * replays each protocol transcript (specs/lsp/README.md) through the
 * JSON-RPC adapter: every client message in order, and every answer must
 * equal the server message the transcript writes next, with no answer the
 * transcript does not write.
 *
 *   lsp_runner --frames
 *
 * reads the protocol's base framing back from streams of every shape a
 * header may take, and from broken ones.
 *
 *   lsp_runner --json
 *
 * holds the JSON reader to RFC 8259: texts it must read and texts it must
 * refuse, and random edits of valid texts, every one of which it either
 * refuses or reads to a value it writes back and reads again unchanged.
 *
 *   lsp_runner --sync
 *
 * is the synchronization oracle: random texts of every line ending and
 * scalar width, edited by random content changes in both position units,
 * against a model of the text as a sequence of scalars. After every change
 * the engine's text must be the model's, a change the model rejects must
 * leave the text as it was, and every position must stand where the model
 * says, at a cursor the parser's context query takes.
 */

#include "adapter.h"
#include "frame.h"
#include "json.h"
#include "markdown_core_lsp.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* The runner links the engine and its static parser and nothing else, so it
 * reads files and draws numbers on its own. */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    uint8_t *bytes = NULL;
    size_t held = 0, room = 0, read;
    do {
        if (held == room) {
            room = room ? room * 2 : 4096;
            uint8_t *grown = (uint8_t *)realloc(bytes, room);
            if (!grown) {
                free(bytes);
                fclose(file);
                return NULL;
            }
            bytes = grown;
        }
        read = fread(bytes + held, 1, room - held, file);
        held += read;
    } while (read > 0);
    fclose(file);
    *size = held;
    return bytes;
}

typedef struct prng_state {
    uint64_t state;
} prng_state;

static void prng_seed(prng_state *prng, uint64_t seed) { prng->state = seed; }

/* SplitMix64. */
static uint64_t prng_next(prng_state *prng) {
    uint64_t z = (prng->state += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

static void print_value(const char *label, const json_tree *tree, const json_value *value) {
    json_out out = {NULL, 0, 0, false};
    json_out_value(&out, tree, value);
    fprintf(stderr, "  %s %.*s\n", label, (int)out.size, out.data ? (const char *)out.data : "");
    json_out_release(&out);
}

static void step_failed(const char *path, size_t index, const char *what) {
    fprintf(stderr, "FAIL %s, message %zu: %s\n", path, index, what);
    failures++;
}

/* Replays one transcript; false when it cannot be read. */
static bool replay(const char *path) {
    size_t size;
    uint8_t *bytes = read_file(path, &size);
    if (!bytes) {
        fprintf(stderr, "FAIL %s: cannot be read\n", path);
        failures++;
        return false;
    }
    json_tree script = {0};
    if (json_read(&script, bytes, size) != JSON_READ_OK) {
        fprintf(stderr, "FAIL %s: is not JSON\n", path);
        failures++;
        free(bytes);
        json_tree_release(&script);
        return false;
    }
    const json_value *messages = json_member(&script, json_root(&script), "messages");
    if (!messages || messages->kind != JSON_ARRAY) {
        fprintf(stderr, "FAIL %s: has no messages\n", path);
        failures++;
        free(bytes);
        json_tree_release(&script);
        return false;
    }
    adapter self;
    if (!adapter_open(&self)) {
        fprintf(stderr, "FAIL %s: the adapter does not open\n", path);
        failures++;
        free(bytes);
        json_tree_release(&script);
        return false;
    }
    json_tree answer = {0};
    bool pending = false, ended = false;
    size_t index = 0;
    int before = failures;
    for (const json_value *step = json_first(&script, messages); step && failures == before;
         step = json_next(&script, messages, step), index++) {
        const json_value *client = json_member(&script, step, "client");
        const json_value *raw = json_member(&script, step, "raw");
        const json_value *server = json_member(&script, step, "server");
        const json_value *exit = json_member(&script, step, "exit");
        if (client || raw) {
            if (pending) {
                step_failed(path, index, "the server answered a message the transcript does not answer");
                print_value("answer", &answer, json_root(&answer));
                break;
            }
            if (ended) {
                step_failed(path, index, "a message follows exit");
                break;
            }
            json_out body = {NULL, 0, 0, false};
            if (client) {
                json_out_value(&body, &script, client);
            } else if (raw->kind == JSON_STRING) {
                json_out_bytes(&body, json_bytes(&script, raw), raw->size);
            }
            adapter_handle(&self, body.data, body.size);
            json_out_release(&body);
            if (self.out.failed) {
                step_failed(path, index, "the answer could not be written");
                break;
            }
            ended = self.exited;
            if (self.out.size) {
                if (json_read(&answer, self.out.data, self.out.size) != JSON_READ_OK) {
                    step_failed(path, index, "the answer is not JSON");
                    break;
                }
                pending = true;
            }
        } else if (server) {
            if (!pending) {
                step_failed(path, index, "the server did not answer");
                print_value("expected", &script, server);
                break;
            }
            if (!json_equal(&script, server, &answer, json_root(&answer))) {
                step_failed(path, index, "the answer differs");
                print_value("expected", &script, server);
                print_value("actual  ", &answer, json_root(&answer));
            }
            pending = false;
        } else if (exit) {
            int64_t code;
            if (!json_integer(&script, exit, 0, 255, &code)) {
                step_failed(path, index, "the exit code is not one");
            } else if (pending) {
                step_failed(path, index, "the server answered a message the transcript does not answer");
                print_value("answer", &answer, json_root(&answer));
            } else if (adapter_code(&self) != (int)code) {
                step_failed(path, index, "the exit code differs");
            }
            ended = true;
        } else {
            step_failed(path, index, "the step is neither client, raw, server nor exit");
        }
    }
    if (failures == before && pending) {
        step_failed(path, index, "the server answered a message the transcript does not answer");
        print_value("answer", &answer, json_root(&answer));
    }
    json_tree_release(&answer);
    adapter_close(&self);
    json_tree_release(&script);
    free(bytes);
    if (failures == before) {
        printf("ok %s (%zu messages)\n", path, index);
    }
    return true;
}

/* THE MODEL: a text as a sequence of scalars, its lines split where the
 * protocol splits them, and a position found by counting scalars. It never
 * looks at a byte. */

typedef struct model {
    uint32_t *scalars;
    size_t count;
    size_t capacity;
} model;

static void model_reserve(model *text, size_t count) {
    if (count <= text->capacity) {
        return;
    }
    size_t capacity = text->capacity ? text->capacity : 64;
    while (capacity < count) {
        capacity *= 2;
    }
    text->scalars = (uint32_t *)realloc(text->scalars, capacity * sizeof(*text->scalars));
    if (!text->scalars) {
        fputs("FAIL: the model cannot grow\n", stderr);
        exit(1);
    }
    text->capacity = capacity;
}

static size_t model_width(uint32_t scalar, markdown_core_text_unit unit) {
    if (unit == MARKDOWN_CORE_TEXT_UNIT_UTF16) {
        return scalar >= 0x10000 ? 2 : 1;
    }
    return scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
}

/* The index of the scalar a position stands before, and the parser's
 * cursor there. */
static size_t model_locate(const model *text, markdown_core_lsp_position position, markdown_core_text_unit unit,
                           markdown_core_position *cursor) {
    size_t at = 0, line = 0;
    while (line < position.line) {
        size_t i = at;
        while (i < text->count && text->scalars[i] != '\n' && text->scalars[i] != '\r') {
            i++;
        }
        if (i == text->count) {
            size_t units = 0;
            for (size_t k = at; k < text->count; k++) {
                units += model_width(text->scalars[k], unit);
            }
            cursor->line = (int32_t)line + 1;
            cursor->column = (int32_t)units + 1;
            return text->count;
        }
        at = i + (text->scalars[i] == '\r' && i + 1 < text->count && text->scalars[i + 1] == '\n' ? 2 : 1);
        line++;
    }
    size_t units = 0;
    while (at < text->count && text->scalars[at] != '\n' && text->scalars[at] != '\r') {
        size_t width = model_width(text->scalars[at], unit);
        if (units + width > position.character) {
            break;
        }
        units += width;
        at++;
    }
    cursor->line = (int32_t)line + 1;
    cursor->column = (int32_t)units + 1;
    return at;
}

static size_t encode(uint32_t scalar, uint8_t *bytes) {
    if (scalar < 0x80) {
        bytes[0] = (uint8_t)scalar;
        return 1;
    }
    if (scalar < 0x800) {
        bytes[0] = (uint8_t)(0xC0 | (scalar >> 6));
        bytes[1] = (uint8_t)(0x80 | (scalar & 0x3F));
        return 2;
    }
    if (scalar < 0x10000) {
        bytes[0] = (uint8_t)(0xE0 | (scalar >> 12));
        bytes[1] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | (scalar & 0x3F));
        return 3;
    }
    bytes[0] = (uint8_t)(0xF0 | (scalar >> 18));
    bytes[1] = (uint8_t)(0x80 | ((scalar >> 12) & 0x3F));
    bytes[2] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
    bytes[3] = (uint8_t)(0x80 | (scalar & 0x3F));
    return 4;
}

/* The scalars `text` encodes, written into `bytes`, and their size. */
static size_t model_bytes(const uint32_t *scalars, size_t count, uint8_t *bytes) {
    size_t size = 0;
    for (size_t i = 0; i < count; i++) {
        size += encode(scalars[i], bytes + size);
    }
    return size;
}

/* Every line ending, every scalar width, and the syntax a cursor's context
 * opens on. */
static const uint32_t ALPHABET[] = {'a', 'b', ' ', '\n', '\r', '[', ']', '(', '#', 0xE9, 0x20AC, 0x4E2D, 0x1F600};
enum { ALPHABET_SIZE = sizeof(ALPHABET) / sizeof(ALPHABET[0]) };

static uint32_t pick_scalar(prng_state *prng) { return ALPHABET[prng_next(prng) % ALPHABET_SIZE]; }

static void fill(prng_state *prng, uint32_t *scalars, size_t count) {
    for (size_t i = 0; i < count; i++) {
        scalars[i] = pick_scalar(prng);
    }
}

/* A position at or near the text: lines and characters run a little past
 * the last, so that clamping is met. */
static markdown_core_lsp_position pick_position(prng_state *prng, const model *text) {
    size_t lines = 1;
    for (size_t i = 0; i < text->count; i++) {
        if (text->scalars[i] == '\n' ||
            (text->scalars[i] == '\r' && !(i + 1 < text->count && text->scalars[i + 1] == '\n'))) {
            lines++;
        }
    }
    markdown_core_lsp_position position;
    position.line = (uint32_t)(prng_next(prng) % (lines + 2));
    position.character = (uint32_t)(prng_next(prng) % 12);
    if (prng_next(prng) % 16 == 0) {
        position.character = UINT32_MAX;
    }
    return position;
}

static const uint8_t SYNC_URI[] = "file:///sync.md";

static void sync_case(prng_state *prng, markdown_core_text_unit unit, size_t index) {
    markdown_core_lsp_server *server;
    markdown_core_lsp_capabilities capabilities;
    if (markdown_core_lsp_server_new(&server) != MARKDOWN_CORE_LSP_OK ||
        markdown_core_lsp_initialize(server, &unit, 1, &capabilities) != MARKDOWN_CORE_LSP_OK ||
        capabilities.unit != unit) {
        fprintf(stderr, "FAIL sync case %zu: the server does not count in the unit it was offered\n", index);
        failures++;
        markdown_core_lsp_server_free(server);
        return;
    }
    model text = {NULL, 0, 0};
    text.count = (size_t)(prng_next(prng) % 24);
    model_reserve(&text, text.count + 1);
    fill(prng, text.scalars, text.count);
    uint8_t *bytes = (uint8_t *)malloc(4 * text.count + 1);
    size_t size = model_bytes(text.scalars, text.count, bytes);
    if (markdown_core_lsp_open(server, SYNC_URI, sizeof(SYNC_URI) - 1, 1, bytes, size) != MARKDOWN_CORE_LSP_OK) {
        fprintf(stderr, "FAIL sync case %zu: the document does not open\n", index);
        failures++;
    }
    free(bytes);
    int32_t current = 1;
    for (int32_t version = 2; version < 18 && !failures; version++) {
        markdown_core_lsp_edit edits[3];
        uint32_t inserted[3][4];
        uint8_t encoded[3][16];
        size_t count = 1 + (size_t)(prng_next(prng) % 3);
        /* The model applies the change to a copy, as the engine must. */
        model next = {NULL, 0, 0};
        model_reserve(&next, text.count + 13);
        for (size_t i = 0; i < text.count; i++) {
            next.scalars[i] = text.scalars[i];
        }
        next.count = text.count;
        bool valid = true;
        for (size_t e = 0; e < count; e++) {
            markdown_core_lsp_scope scope;
            if (prng_next(prng) % 10 == 0) {
                scope.start.line = scope.start.character = 0;
                scope.end.line = scope.end.character = UINT32_MAX;
            } else {
                scope.start = pick_position(prng, &next);
                scope.end = prng_next(prng) % 3 == 0 ? scope.start : pick_position(prng, &next);
            }
            size_t added = (size_t)(prng_next(prng) % 5);
            fill(prng, inserted[e], added);
            edits[e].scope = scope;
            edits[e].size = model_bytes(inserted[e], added, encoded[e]);
            edits[e].text = encoded[e];
            markdown_core_position cursor;
            size_t start = model_locate(&next, scope.start, unit, &cursor);
            size_t end = model_locate(&next, scope.end, unit, &cursor);
            if (!valid) {
                continue;
            }
            if (start > end) {
                valid = false;
                continue;
            }
            model_reserve(&next, next.count - (end - start) + added);
            memmove(next.scalars + start + added, next.scalars + end, (next.count - end) * sizeof(*next.scalars));
            memcpy(next.scalars + start, inserted[e], added * sizeof(*next.scalars));
            next.count = next.count - (end - start) + added;
        }
        markdown_core_lsp_status status =
            markdown_core_lsp_update(server, SYNC_URI, sizeof(SYNC_URI) - 1, version, edits, count);
        markdown_core_lsp_status expected = valid ? MARKDOWN_CORE_LSP_OK : MARKDOWN_CORE_LSP_INVALID_PARAMS;
        if (status != expected) {
            fprintf(stderr, "FAIL sync case %zu, version %d: the update answers %d, the model %d\n", index,
                    (int)version, (int)status, (int)expected);
            failures++;
        }
        if (valid) {
            free(text.scalars);
            text = next;
            current = version;
        } else {
            free(next.scalars);
        }
        const lsp_text *held = lsp_server_text(server, SYNC_URI, sizeof(SYNC_URI) - 1);
        uint8_t *want = (uint8_t *)malloc(4 * text.count + 1);
        size_t want_size = model_bytes(text.scalars, text.count, want);
        if (!held || held->size != want_size || (want_size && memcmp(held->bytes, want, want_size) != 0)) {
            fprintf(stderr, "FAIL sync case %zu, version %d: the text differs from the model's\n", index, (int)version);
            failures++;
        }
        /* Every position stands where the model says, at a cursor the
         * context query takes. */
        markdown_core_document *document = NULL;
        if (held && markdown_core_document_parse_in(held->bytes, held->size, unit, &document) != MARKDOWN_CORE_OK) {
            fprintf(stderr, "FAIL sync case %zu: the text does not parse\n", index);
            failures++;
            held = NULL;
        }
        for (int p = 0; held && p < 8 && !failures; p++) {
            markdown_core_lsp_position position = pick_position(prng, &text);
            markdown_core_position cursor, model_cursor;
            size_t offset;
            lsp_text_locate(held, position, unit, &offset, &cursor);
            size_t at = model_locate(&text, position, unit, &model_cursor);
            size_t model_offset = model_bytes(text.scalars, at, want);
            markdown_core_context *context;
            if (offset != model_offset || cursor.line != model_cursor.line || cursor.column != model_cursor.column) {
                fprintf(stderr, "FAIL sync case %zu: %u:%u stands at byte %zu (%d:%d), the model's %zu (%d:%d)\n",
                        index, position.line, position.character, offset, cursor.line, cursor.column, model_offset,
                        model_cursor.line, model_cursor.column);
                failures++;
            } else if (markdown_core_document_context(document, held->bytes, held->size, cursor, &context) !=
                       MARKDOWN_CORE_OK) {
                fprintf(stderr, "FAIL sync case %zu: the context query refuses the cursor %d:%d\n", index, cursor.line,
                        cursor.column);
                failures++;
            } else {
                markdown_core_context_free(context);
            }
            markdown_core_lsp_list *list;
            if (markdown_core_lsp_complete(server, SYNC_URI, sizeof(SYNC_URI) - 1, position, &list) !=
                MARKDOWN_CORE_LSP_OK) {
                fprintf(stderr, "FAIL sync case %zu: completion fails at %u:%u\n", index, position.line,
                        position.character);
                failures++;
            } else {
                if (markdown_core_lsp_list_version(list) != current) {
                    fprintf(stderr, "FAIL sync case %zu: the list names version %d, not %d\n", index,
                            (int)markdown_core_lsp_list_version(list), (int)current);
                    failures++;
                }
                markdown_core_lsp_list_free(list);
            }
        }
        markdown_core_document_free(document);
        free(want);
    }
    if (markdown_core_lsp_close(server, SYNC_URI, sizeof(SYNC_URI) - 1) != MARKDOWN_CORE_LSP_OK ||
        lsp_server_text(server, SYNC_URI, sizeof(SYNC_URI) - 1) != NULL) {
        fprintf(stderr, "FAIL sync case %zu: the document does not close\n", index);
        failures++;
    }
    free(text.scalars);
    markdown_core_lsp_server_free(server);
}

/* The store under many documents: every open one is found with its own
 * text, and every closed one is gone, whichever slots their URIs probe. */
static void store_case(prng_state *prng) {
    enum { STORE_COUNT = 300 };
    markdown_core_lsp_server *server;
    markdown_core_lsp_capabilities capabilities;
    if (markdown_core_lsp_server_new(&server) != MARKDOWN_CORE_LSP_OK ||
        markdown_core_lsp_initialize(server, NULL, 0, &capabilities) != MARKDOWN_CORE_LSP_OK) {
        fprintf(stderr, "FAIL store: the server does not start\n");
        failures++;
        markdown_core_lsp_server_free(server);
        return;
    }
    bool open[STORE_COUNT] = {false};
    char uri[32];
    for (int round = 0; round < 4000 && !failures; round++) {
        int which = (int)(prng_next(prng) % STORE_COUNT);
        int size = snprintf(uri, sizeof(uri), "file:///%d.md", which);
        if (!open[which] || prng_next(prng) % 2) {
            markdown_core_lsp_open(server, (const uint8_t *)uri, (size_t)size, round, (const uint8_t *)uri,
                                   (size_t)size);
            open[which] = true;
        } else {
            markdown_core_lsp_close(server, (const uint8_t *)uri, (size_t)size);
            open[which] = false;
        }
        for (int k = 0; k < STORE_COUNT; k++) {
            int known = snprintf(uri, sizeof(uri), "file:///%d.md", k);
            const lsp_text *held = lsp_server_text(server, (const uint8_t *)uri, (size_t)known);
            bool right = open[k] ? held && held->size == (size_t)known && !memcmp(held->bytes, uri, (size_t)known)
                                 : held == NULL;
            if (!right) {
                fprintf(stderr, "FAIL store round %d: %s is %s\n", round, uri,
                        open[k] ? "open but not found" : "closed but found");
                failures++;
                break;
            }
        }
    }
    markdown_core_lsp_server_free(server);
}

/* One framed stream and what reading it gives: the bodies read in order,
 * then the status that ends the stream. */
typedef struct frame_case {
    const char *name;
    const char *stream;
    size_t size;
    const char *bodies[3];
    frame_status last;
} frame_case;

#define STREAM(text) text, sizeof(text) - 1

static const frame_case FRAME_CASES[] = {
    {"one message", STREAM("Content-Length: 2\r\n\r\n{}"), {"{}"}, FRAME_END},
    {"two messages", STREAM("Content-Length: 2\r\n\r\n{}Content-Length: 4\r\n\r\nnull"), {"{}", "null"}, FRAME_END},
    {"any case and other fields",
     STREAM("content-type: application/vscode-jsonrpc; charset=utf-8\r\nCONTENT-LENGTH:3\r\n\r\n[1]"),
     {"[1]"},
     FRAME_END},
    {"an empty body", STREAM("Content-Length: 0\r\n\r\n"), {""}, FRAME_END},
    {"no input", STREAM(""), {NULL}, FRAME_END},
    {"no length", STREAM("Content-Type: x\r\n\r\n{}"), {NULL}, FRAME_BROKEN},
    {"a bare line feed", STREAM("Content-Length: 2\n\r\n{}"), {NULL}, FRAME_BROKEN},
    {"a length that is not a number", STREAM("Content-Length: 2a\r\n\r\n{}"), {NULL}, FRAME_BROKEN},
    {"an empty length", STREAM("Content-Length:\r\n\r\n{}"), {NULL}, FRAME_BROKEN},
    {"a length past every body", STREAM("Content-Length: 99999999999999999999\r\n\r\n"), {NULL}, FRAME_BROKEN},
    {"a short body", STREAM("Content-Length: 5\r\n\r\n{}"), {NULL}, FRAME_BROKEN},
    {"a header cut off", STREAM("Content-Length: 2\r\n"), {NULL}, FRAME_BROKEN},
};

/* A stream on a file of the working directory: tmpfile() may write where a
 * test cannot, on some platforms the drive's root. */
static FILE *scratch(void) { return fopen("lsp_frames.scratch", "w+b"); }

static void frame_cases(void) {
    for (size_t i = 0; i < sizeof(FRAME_CASES) / sizeof(FRAME_CASES[0]); i++) {
        const frame_case *test = &FRAME_CASES[i];
        FILE *stream = scratch();
        if (!stream || (test->size && fwrite(test->stream, 1, test->size, stream) != test->size)) {
            fprintf(stderr, "FAIL frames: no temporary stream\n");
            failures++;
            if (stream) {
                fclose(stream);
            }
            return;
        }
        rewind(stream);
        frame message = {NULL, 0, 0};
        size_t read = 0;
        frame_status status;
        while ((status = frame_read(stream, &message)) == FRAME_OK) {
            const char *want = read < 3 ? test->bodies[read] : NULL;
            if (!want || message.size != strlen(want) || (message.size && memcmp(message.body, want, message.size))) {
                fprintf(stderr, "FAIL frames, %s: body %zu is \"%.*s\"\n", test->name, read, (int)message.size,
                        (const char *)message.body);
                failures++;
            }
            read++;
        }
        if (status != test->last || (read < 3 && test->bodies[read])) {
            fprintf(stderr, "FAIL frames, %s: the stream ends with %d after %zu bodies\n", test->name, (int)status,
                    read);
            failures++;
        }
        frame_release(&message);
        fclose(stream);
    }
    /* What frame_write writes, frame_read reads. */
    FILE *stream = scratch();
    if (stream) {
        frame_write(stream, (const uint8_t *)"{\"a\":1}", 7);
        frame_write(stream, NULL, 0);
        rewind(stream);
        frame message = {NULL, 0, 0};
        if (frame_read(stream, &message) != FRAME_OK || message.size != 7 ||
            memcmp(message.body, "{\"a\":1}", 7) != 0 || frame_read(stream, &message) != FRAME_OK ||
            message.size != 0 || frame_read(stream, &message) != FRAME_END) {
            fprintf(stderr, "FAIL frames: a written stream does not read back\n");
            failures++;
        }
        frame_release(&message);
        fclose(stream);
    }
    remove("lsp_frames.scratch");
    if (!failures) {
        printf("ok frames: %zu streams\n", sizeof(FRAME_CASES) / sizeof(FRAME_CASES[0]) + 1);
    }
}

typedef struct json_case {
    const char *text;
    json_read_status status;
    /* How the value is written back, when it reads. */
    const char *written;
} json_case;

static const json_case JSON_CASES[] = {
    {"null", JSON_READ_OK, "null"},
    {" \t\r\n true \n", JSON_READ_OK, "true"},
    {"[]", JSON_READ_OK, "[]"},
    {"{}", JSON_READ_OK, "{}"},
    {"[1, -0, 0.5, -1.25e+3, 2E-2, 10]", JSON_READ_OK, "[1,-0,0.5,-1.25e+3,2E-2,10]"},
    {"{\"a\":{\"b\":[null,false]},\"c\":\"d\"}", JSON_READ_OK, "{\"a\":{\"b\":[null,false]},\"c\":\"d\"}"},
    {"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", JSON_READ_OK, "\"\\\"\\\\/\\b\\f\\n\\r\\t\""},
    {"\"\\u0041\\u00e9\\u20AC\\ud83d\\ude00\"", JSON_READ_OK, "\"A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\""},
    {"\"\\ud800\"", JSON_READ_OK, "\"\xef\xbf\xbd\""},
    {"\"\\udc00\\ud800x\"", JSON_READ_OK, "\"\xef\xbf\xbd\xef\xbf\xbdx\""},
    {"\"\\u0000\\u001f\"", JSON_READ_OK, "\"\\u0000\\u001f\""},
    {"\"\xc3\xa9\"", JSON_READ_OK, "\"\xc3\xa9\""},
    {"", JSON_READ_MALFORMED, NULL},
    {" ", JSON_READ_MALFORMED, NULL},
    {"[1,]", JSON_READ_MALFORMED, NULL},
    {"[1 2]", JSON_READ_MALFORMED, NULL},
    {"{\"a\" 1}", JSON_READ_MALFORMED, NULL},
    {"{\"a\":1,}", JSON_READ_MALFORMED, NULL},
    {"{1:1}", JSON_READ_MALFORMED, NULL},
    {"[01]", JSON_READ_MALFORMED, NULL},
    {"[1.]", JSON_READ_MALFORMED, NULL},
    {"[.5]", JSON_READ_MALFORMED, NULL},
    {"[1e]", JSON_READ_MALFORMED, NULL},
    {"[-]", JSON_READ_MALFORMED, NULL},
    {"[+1]", JSON_READ_MALFORMED, NULL},
    {"[NaN]", JSON_READ_MALFORMED, NULL},
    {"[tru]", JSON_READ_MALFORMED, NULL},
    {"\"a", JSON_READ_MALFORMED, NULL},
    {"\"\t\"", JSON_READ_MALFORMED, NULL},
    {"\"\\x\"", JSON_READ_MALFORMED, NULL},
    {"\"\\u12\"", JSON_READ_MALFORMED, NULL},
    {"\"\\u12g4\"", JSON_READ_MALFORMED, NULL},
    {"[1]]", JSON_READ_MALFORMED, NULL},
    {"1 2", JSON_READ_MALFORMED, NULL},
    {"'a'", JSON_READ_MALFORMED, NULL},
    {"[\"a\"\x00]", JSON_READ_MALFORMED, NULL},
};

static bool reads_back(json_tree *tree, json_tree *again, const uint8_t *text, size_t size, json_read_status *status) {
    *status = json_read(tree, text, size);
    if (*status != JSON_READ_OK) {
        return true;
    }
    json_out out = {NULL, 0, 0, false};
    json_out_value(&out, tree, json_root(tree));
    bool same = !out.failed && json_read(again, out.data, out.size) == JSON_READ_OK &&
                json_equal(tree, json_root(tree), again, json_root(again));
    json_out_release(&out);
    return same;
}

static void json_cases(void) {
    json_tree tree = {0}, again = {0};
    size_t count = sizeof(JSON_CASES) / sizeof(JSON_CASES[0]);
    for (size_t i = 0; i < count; i++) {
        const json_case *test = &JSON_CASES[i];
        /* The table's texts end at their NUL, except the one that holds one. */
        size_t size = strlen(test->text);
        if (strcmp(test->text, "[\"a\"") == 0) {
            size = 6;
        }
        json_read_status status = json_read(&tree, (const uint8_t *)test->text, size);
        if (status != test->status) {
            fprintf(stderr, "FAIL json %zu: %s reads %d, not %d\n", i, test->text, (int)status, (int)test->status);
            failures++;
            continue;
        }
        if (status == JSON_READ_OK) {
            json_out out = {NULL, 0, 0, false};
            json_out_value(&out, &tree, json_root(&tree));
            if (out.size != strlen(test->written) || memcmp(out.data, test->written, out.size) != 0) {
                fprintf(stderr, "FAIL json %zu: %s writes %.*s, not %s\n", i, test->text, (int)out.size,
                        (const char *)out.data, test->written);
                failures++;
            }
            json_out_release(&out);
        }
    }
    /* Nesting up to the limit reads, and past it is refused. */
    for (size_t depth = 511; depth <= 513; depth++) {
        char *text = (char *)malloc(2 * depth);
        memset(text, '[', depth);
        memset(text + depth, ']', depth);
        json_read_status status = json_read(&tree, (const uint8_t *)text, 2 * depth);
        if (status != (depth <= 512 ? JSON_READ_OK : JSON_READ_MALFORMED)) {
            fprintf(stderr, "FAIL json: %zu nested arrays read %d\n", depth, (int)status);
            failures++;
        }
        free(text);
    }
    /* Random edits of valid texts: each is refused, or reads back. */
    prng_state prng;
    prng_seed(&prng, 0x6a736f6eu);
    static const char EDIT[] = "{}[]\":,\\ -.0123eEtrufalsn\x01\x80\xff";
    size_t mutated = 0;
    for (size_t i = 0; i < count && !failures; i++) {
        if (JSON_CASES[i].status != JSON_READ_OK) {
            continue;
        }
        size_t size = strlen(JSON_CASES[i].text);
        for (int round = 0; round < 2000 && !failures; round++, mutated++) {
            uint8_t text[96];
            memcpy(text, JSON_CASES[i].text, size);
            size_t held = size;
            for (int k = 1 + (int)(prng_next(&prng) % 3); k > 0; k--) {
                size_t at = held ? (size_t)(prng_next(&prng) % held) : 0;
                uint8_t byte = (uint8_t)EDIT[prng_next(&prng) % (sizeof(EDIT) - 1)];
                switch (prng_next(&prng) % 3) {
                case 0:
                    if (held) {
                        text[at] = byte;
                    }
                    break;
                case 1:
                    if (held < sizeof(text)) {
                        memmove(text + at + 1, text + at, held - at);
                        text[at] = byte;
                        held++;
                    }
                    break;
                default:
                    if (held) {
                        memmove(text + at, text + at + 1, held - at - 1);
                        held--;
                    }
                    break;
                }
            }
            json_read_status status;
            if (!reads_back(&tree, &again, text, held, &status) || status == JSON_READ_FAILED) {
                fprintf(stderr, "FAIL json: %.*s does not read back\n", (int)held, (const char *)text);
                failures++;
            }
        }
    }
    json_tree_release(&tree);
    json_tree_release(&again);
    if (!failures) {
        printf("ok json: %zu texts, the nesting limit, and %zu edited texts\n", count, mutated);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--transcripts") == 0) {
        for (int i = 2; i < argc; i++) {
            replay(argv[i]);
        }
    } else if (argc == 2 && strcmp(argv[1], "--json") == 0) {
        json_cases();
    } else if (argc == 2 && strcmp(argv[1], "--frames") == 0) {
        frame_cases();
    } else if (argc == 2 && strcmp(argv[1], "--sync") == 0) {
        prng_state prng;
        prng_seed(&prng, 0x6c7370u);
        size_t cases = 0;
        for (size_t i = 0; i < 3000 && !failures; i++, cases++) {
            sync_case(&prng, i % 2 ? MARKDOWN_CORE_TEXT_UNIT_UTF8 : MARKDOWN_CORE_TEXT_UNIT_UTF16, i);
        }
        store_case(&prng);
        if (!failures) {
            printf("ok sync: %zu edited texts in both units, and the store\n", cases);
        }
    } else {
        fprintf(stderr, "usage: lsp_runner --transcripts FILE... | --json | --frames | --sync\n");
        return 2;
    }
    return failures ? 1 : 0;
}
