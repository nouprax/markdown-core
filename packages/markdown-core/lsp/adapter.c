#include "adapter.h"

#include <stdlib.h>
#include <string.h>

/* The protocol's own error codes, which the engine's statuses do not name
 * because only a message can be malformed. */
enum {
    RPC_PARSE_ERROR = -32700,
    RPC_INVALID_REQUEST = -32600,
    RPC_METHOD_NOT_FOUND = -32601,
    RPC_INVALID_PARAMS = -32602,
    RPC_INTERNAL_ERROR = -32603
};

static const char *reason(int code) {
    switch (code) {
    case RPC_PARSE_ERROR:
        return "the message is not JSON";
    case RPC_INVALID_REQUEST:
        return "the message is not a request the server takes now";
    case RPC_METHOD_NOT_FOUND:
        return "the server has no such method";
    case RPC_INVALID_PARAMS:
        return "the params are not the method's";
    case MARKDOWN_CORE_LSP_SERVER_NOT_INITIALIZED:
        return "the server is not initialized";
    case RPC_INTERNAL_ERROR:
    default:
        return "the server failed";
    }
}

bool adapter_open(adapter *self) {
    memset(self, 0, sizeof(*self));
    return markdown_core_lsp_server_new(&self->server) == MARKDOWN_CORE_LSP_OK;
}

void adapter_close(adapter *self) {
    markdown_core_lsp_server_free(self->server);
    json_tree_release(&self->tree);
    json_out_release(&self->out);
    free(self->edits);
    memset(self, 0, sizeof(*self));
}

int adapter_code(const adapter *self) {
    return markdown_core_lsp_server_phase(self->server) == MARKDOWN_CORE_LSP_HALTED ? 0 : 1;
}

/* Starts an answer to the request `id`, NULL for none. */
static void answer(adapter *self, const json_value *id) {
    json_out_raw(&self->out, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id) {
        json_out_value(&self->out, &self->tree, id);
    } else {
        json_out_raw(&self->out, "null");
    }
}

static void answer_error(adapter *self, const json_value *id, int code) {
    answer(self, id);
    json_out_raw(&self->out, ",\"error\":{\"code\":");
    json_out_integer(&self->out, code);
    json_out_raw(&self->out, ",\"message\":");
    json_out_text(&self->out, reason(code));
    json_out_raw(&self->out, "}}");
}

static bool read_position(const json_tree *tree, const json_value *value, markdown_core_lsp_position *position) {
    int64_t line, character;
    if (!json_integer(tree, json_member(tree, value, "line"), 0, UINT32_MAX, &line) ||
        !json_integer(tree, json_member(tree, value, "character"), 0, UINT32_MAX, &character)) {
        return false;
    }
    position->line = (uint32_t)line;
    position->character = (uint32_t)character;
    return true;
}

static bool read_scope(const json_tree *tree, const json_value *value, markdown_core_lsp_scope *scope) {
    return read_position(tree, json_member(tree, value, "start"), &scope->start) &&
           read_position(tree, json_member(tree, value, "end"), &scope->end);
}

/* The `textDocument.uri` of `params`, or NULL. */
static const json_value *read_uri(const json_tree *tree, const json_value *params) {
    const json_value *uri = json_member(tree, json_member(tree, params, "textDocument"), "uri");
    return uri && uri->kind == JSON_STRING ? uri : NULL;
}

static bool read_version(const json_tree *tree, const json_value *params, int32_t *version) {
    int64_t read;
    if (!json_integer(tree, json_member(tree, json_member(tree, params, "textDocument"), "version"), INT32_MIN,
                      INT32_MAX, &read)) {
        return false;
    }
    *version = (int32_t)read;
    return true;
}

static int initialize(adapter *self, const json_value *id, const json_value *params) {
    const json_tree *tree = &self->tree;
    if (!params || params->kind != JSON_OBJECT) {
        return RPC_INVALID_PARAMS;
    }
    markdown_core_text_unit units[2];
    size_t count = 0;
    const json_value *offered =
        json_member(tree, json_member(tree, json_member(tree, params, "capabilities"), "general"), "positionEncodings");
    if (offered && offered->kind == JSON_ARRAY) {
        for (const json_value *name = json_first(tree, offered); name; name = json_next(tree, offered, name)) {
            if (count < 2 && json_is(tree, name, "utf-8")) {
                units[count++] = MARKDOWN_CORE_TEXT_UNIT_UTF8;
            } else if (count < 2 && json_is(tree, name, "utf-16")) {
                units[count++] = MARKDOWN_CORE_TEXT_UNIT_UTF16;
            }
        }
    }
    markdown_core_lsp_capabilities capabilities;
    markdown_core_lsp_status status = markdown_core_lsp_initialize(self->server, units, count, &capabilities);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    json_out *out = &self->out;
    answer(self, id);
    json_out_raw(out, ",\"result\":{\"capabilities\":{\"positionEncoding\":");
    json_out_text(out, capabilities.unit == MARKDOWN_CORE_TEXT_UNIT_UTF8 ? "utf-8" : "utf-16");
    json_out_raw(out, ",\"textDocumentSync\":{\"openClose\":true,\"change\":2}");
    json_out_raw(out, ",\"completionProvider\":{\"triggerCharacters\":[");
    for (size_t i = 0; i < capabilities.cues.length; i++) {
        if (i) {
            json_out_raw(out, ",");
        }
        json_out_string(out, capabilities.cues.data + i, 1);
    }
    json_out_raw(out, "]}},\"serverInfo\":{\"name\":\"markdown-core\"}}}");
    return MARKDOWN_CORE_LSP_OK;
}

static int complete(adapter *self, const json_value *id, const json_value *params) {
    const json_tree *tree = &self->tree;
    const json_value *uri = read_uri(tree, params);
    markdown_core_lsp_position position;
    if (!uri || !read_position(tree, json_member(tree, params, "position"), &position)) {
        return RPC_INVALID_PARAMS;
    }
    markdown_core_lsp_list *list;
    markdown_core_lsp_status status =
        markdown_core_lsp_complete(self->server, json_bytes(tree, uri), uri->size, position, &list);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    markdown_core_lsp_list_free(list);
    answer(self, id);
    json_out_raw(&self->out, ",\"result\":{\"isIncomplete\":false,\"items\":[]}}");
    return MARKDOWN_CORE_LSP_OK;
}

static void request(adapter *self, const json_value *id, const json_value *method, const json_value *params) {
    const json_tree *tree = &self->tree;
    int code;
    if (json_is(tree, method, "initialize")) {
        code = initialize(self, id, params);
    } else if (json_is(tree, method, "shutdown")) {
        code = markdown_core_lsp_shutdown(self->server);
        if (code == MARKDOWN_CORE_LSP_OK) {
            answer(self, id);
            json_out_raw(&self->out, ",\"result\":null}");
        }
    } else if (json_is(tree, method, "textDocument/completion")) {
        code = complete(self, id, params);
    } else {
        /* A request the server does not serve still answers as its phase
         * would: before initialize and after shutdown the phase's error. */
        switch (markdown_core_lsp_server_phase(self->server)) {
        case MARKDOWN_CORE_LSP_FRESH:
            code = MARKDOWN_CORE_LSP_SERVER_NOT_INITIALIZED;
            break;
        case MARKDOWN_CORE_LSP_HALTED:
            code = RPC_INVALID_REQUEST;
            break;
        default:
            code = RPC_METHOD_NOT_FOUND;
            break;
        }
    }
    if (code != MARKDOWN_CORE_LSP_OK) {
        answer_error(self, id, code);
    }
}

static void open_document(adapter *self, const json_value *params) {
    const json_tree *tree = &self->tree;
    const json_value *uri = read_uri(tree, params);
    const json_value *text = json_member(tree, json_member(tree, params, "textDocument"), "text");
    int32_t version;
    if (uri && text && text->kind == JSON_STRING && read_version(tree, params, &version)) {
        markdown_core_lsp_open(self->server, json_bytes(tree, uri), uri->size, version, json_bytes(tree, text),
                               text->size);
    }
}

static void update_document(adapter *self, const json_value *params) {
    const json_tree *tree = &self->tree;
    const json_value *uri = read_uri(tree, params);
    const json_value *changes = json_member(tree, params, "contentChanges");
    int32_t version;
    if (!uri || !changes || changes->kind != JSON_ARRAY || !read_version(tree, params, &version)) {
        return;
    }
    size_t count = 0;
    for (const json_value *change = json_first(tree, changes); change; change = json_next(tree, changes, change)) {
        const json_value *text = json_member(tree, change, "text");
        const json_value *range = json_member(tree, change, "range");
        if (!text || text->kind != JSON_STRING) {
            return;
        }
        if (count == self->room) {
            size_t room = self->room ? self->room * 2 : 8;
            markdown_core_lsp_edit *edits = room > SIZE_MAX / sizeof(*edits)
                                                ? NULL
                                                : (markdown_core_lsp_edit *)realloc(self->edits, room * sizeof(*edits));
            if (!edits) {
                return;
            }
            self->edits = edits;
            self->room = room;
        }
        markdown_core_lsp_edit *edit = &self->edits[count++];
        if (!range) {
            /* No range: the whole text. */
            edit->scope.start.line = edit->scope.start.character = 0;
            edit->scope.end.line = edit->scope.end.character = UINT32_MAX;
        } else if (!read_scope(tree, range, &edit->scope)) {
            return;
        }
        edit->text = json_bytes(tree, text);
        edit->size = text->size;
    }
    markdown_core_lsp_update(self->server, json_bytes(tree, uri), uri->size, version, self->edits, count);
}

/* A notification answers nothing; one that fails is dropped, as the
 * protocol drops one the server cannot take. */
static void notification(adapter *self, const json_value *method, const json_value *params) {
    const json_tree *tree = &self->tree;
    if (json_is(tree, method, "exit")) {
        self->exited = true;
    } else if (json_is(tree, method, "textDocument/didOpen")) {
        open_document(self, params);
    } else if (json_is(tree, method, "textDocument/didChange")) {
        update_document(self, params);
    } else if (json_is(tree, method, "textDocument/didClose")) {
        const json_value *uri = read_uri(tree, params);
        if (uri) {
            markdown_core_lsp_close(self->server, json_bytes(tree, uri), uri->size);
        }
    }
}

void adapter_handle(adapter *self, const uint8_t *body, size_t size) {
    self->out.size = 0;
    self->out.failed = false;
    self->exited = false;
    json_read_status read = json_read(&self->tree, body, size);
    if (read != JSON_READ_OK) {
        answer_error(self, NULL, read == JSON_READ_MALFORMED ? RPC_PARSE_ERROR : RPC_INTERNAL_ERROR);
        return;
    }
    const json_tree *tree = &self->tree;
    const json_value *message = json_root(tree);
    const json_value *id = json_member(tree, message, "id");
    const json_value *method = json_member(tree, message, "method");
    bool valid_id = id && (id->kind == JSON_NUMBER || id->kind == JSON_STRING);
    if (message->kind == JSON_OBJECT && !method && id &&
        (json_member(tree, message, "result") || json_member(tree, message, "error"))) {
        /* A response: the server sends no requests, so none is awaited. */
        return;
    }
    if (message->kind != JSON_OBJECT || !json_is(tree, json_member(tree, message, "jsonrpc"), "2.0") || !method ||
        method->kind != JSON_STRING || (id && !valid_id)) {
        answer_error(self, valid_id ? id : NULL, RPC_INVALID_REQUEST);
        return;
    }
    const json_value *params = json_member(tree, message, "params");
    if (id) {
        request(self, id, method, params);
    } else {
        notification(self, method, params);
    }
}
