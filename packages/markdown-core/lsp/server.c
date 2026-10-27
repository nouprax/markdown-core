#include "markdown_core_lsp.h"

#include "text.h"

#include <stdlib.h>
#include <string.h>

/*
 * The engine's server: the lifecycle, the negotiated unit and the document
 * store (plan, sections 4.3 and 6).
 *
 * A RECORD is one open document: its URI, its version, its text, and the
 * document parsed from that text, which the first request that needs it
 * makes and every edit drops, so a version is parsed at most once.
 *
 * The store is an open-addressed table of records keyed by URI, probed
 * linearly; a close shifts the records after it back, so the table holds no
 * tombstones and a lookup stops at the first empty slot.
 */

typedef struct record {
    uint8_t *uri;
    size_t uri_size;
    uint64_t hash;
    int32_t version;
    lsp_text text;
    markdown_core_document *document;
} record;

struct markdown_core_lsp_server {
    markdown_core_lsp_phase phase;
    markdown_core_text_unit unit;
    record **slots;
    size_t capacity;
    size_t count;
    /* The text an update edits, swapped with the record's when every edit
     * applies, so a failed update leaves the record as it was. */
    lsp_text spare;
};

struct markdown_core_lsp_list {
    int32_t version;
    size_t count;
};

/* FNV-1a. */
static uint64_t hash_uri(const uint8_t *uri, size_t size) {
    uint64_t hash = 14695981039346656037u;
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ uri[i]) * 1099511628211u;
    }
    return hash;
}

/* The slot that holds the record for `uri`, or the empty slot where it
 * would go. The table always has an empty slot. */
static size_t probe(const markdown_core_lsp_server *server, const uint8_t *uri, size_t size, uint64_t hash) {
    size_t mask = server->capacity - 1;
    size_t at = (size_t)hash & mask;
    for (;;) {
        const record *held = server->slots[at];
        if (!held || (held->hash == hash && held->uri_size == size && (size == 0 || !memcmp(held->uri, uri, size)))) {
            return at;
        }
        at = (at + 1) & mask;
    }
}

static record *find(const markdown_core_lsp_server *server, const uint8_t *uri, size_t size) {
    return server->slots[probe(server, uri, size, hash_uri(uri, size))];
}

static void release(record *held) {
    if (!held) {
        return;
    }
    markdown_core_document_free(held->document);
    lsp_text_release(&held->text);
    free(held->uri);
    free(held);
}

/* Doubles the table, which stays at most three quarters full. */
static bool widen(markdown_core_lsp_server *server) {
    size_t capacity = server->capacity * 2;
    record **slots = (record **)calloc(capacity, sizeof(*slots));
    if (!slots) {
        return false;
    }
    record **old = server->slots;
    size_t old_capacity = server->capacity;
    server->slots = slots;
    server->capacity = capacity;
    for (size_t i = 0; i < old_capacity; i++) {
        if (old[i]) {
            slots[probe(server, old[i]->uri, old[i]->uri_size, old[i]->hash)] = old[i];
        }
    }
    free(old);
    return true;
}

/* Empties slot `at` and moves back each record after it that its probe
 * would no longer reach. */
static void vacate(markdown_core_lsp_server *server, size_t at) {
    size_t mask = server->capacity - 1;
    server->slots[at] = NULL;
    for (size_t next = (at + 1) & mask; server->slots[next]; next = (next + 1) & mask) {
        size_t home = (size_t)server->slots[next]->hash & mask;
        /* The record stays when its home lies cyclically in (at, next]. */
        bool stays = at <= next ? (at < home && home <= next) : (at < home || home <= next);
        if (!stays) {
            server->slots[at] = server->slots[next];
            server->slots[next] = NULL;
            at = next;
        }
    }
}

/* Whether the server takes a call that needs an initialized server. */
static markdown_core_lsp_status active(const markdown_core_lsp_server *server) {
    switch (server->phase) {
    case MARKDOWN_CORE_LSP_FRESH:
        return MARKDOWN_CORE_LSP_SERVER_NOT_INITIALIZED;
    case MARKDOWN_CORE_LSP_ACTIVE:
        return MARKDOWN_CORE_LSP_OK;
    case MARKDOWN_CORE_LSP_HALTED:
    default:
        return MARKDOWN_CORE_LSP_INVALID_REQUEST;
    }
}

markdown_core_lsp_status markdown_core_lsp_server_new(markdown_core_lsp_server **server) {
    markdown_core_lsp_server *made = (markdown_core_lsp_server *)calloc(1, sizeof(*made));
    if (!made) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    made->capacity = 8;
    made->slots = (record **)calloc(made->capacity, sizeof(*made->slots));
    if (!made->slots) {
        free(made);
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    made->phase = MARKDOWN_CORE_LSP_FRESH;
    made->unit = MARKDOWN_CORE_TEXT_UNIT_UTF16;
    *server = made;
    return MARKDOWN_CORE_LSP_OK;
}

void markdown_core_lsp_server_free(markdown_core_lsp_server *server) {
    if (!server) {
        return;
    }
    for (size_t i = 0; i < server->capacity; i++) {
        release(server->slots[i]);
    }
    free(server->slots);
    lsp_text_release(&server->spare);
    free(server);
}

markdown_core_lsp_phase markdown_core_lsp_server_phase(const markdown_core_lsp_server *server) { return server->phase; }

markdown_core_lsp_status markdown_core_lsp_initialize(markdown_core_lsp_server *server,
                                                      const markdown_core_text_unit *units, size_t count,
                                                      markdown_core_lsp_capabilities *capabilities) {
    if (server->phase != MARKDOWN_CORE_LSP_FRESH) {
        return MARKDOWN_CORE_LSP_INVALID_REQUEST;
    }
    server->unit = MARKDOWN_CORE_TEXT_UNIT_UTF16;
    for (size_t i = 0; i < count; i++) {
        if (units[i] == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
            server->unit = MARKDOWN_CORE_TEXT_UNIT_UTF8;
        }
    }
    server->phase = MARKDOWN_CORE_LSP_ACTIVE;
    capabilities->unit = server->unit;
    capabilities->cues = markdown_core_slot_cues();
    return MARKDOWN_CORE_LSP_OK;
}

markdown_core_lsp_status markdown_core_lsp_shutdown(markdown_core_lsp_server *server) {
    markdown_core_lsp_status status = active(server);
    if (status == MARKDOWN_CORE_LSP_OK) {
        server->phase = MARKDOWN_CORE_LSP_HALTED;
    }
    return status;
}

markdown_core_lsp_status markdown_core_lsp_open(markdown_core_lsp_server *server, const uint8_t *uri, size_t uri_size,
                                                int32_t version, const uint8_t *text, size_t text_size) {
    markdown_core_lsp_status status = active(server);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    if (server->count + 1 > server->capacity / 4 * 3 && !widen(server)) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    uint64_t hash = hash_uri(uri, uri_size);
    size_t at = probe(server, uri, uri_size, hash);
    record *made = (record *)calloc(1, sizeof(*made));
    if (!made || !lsp_text_assign(&made->text, text, text_size) ||
        !(made->uri = (uint8_t *)malloc(uri_size ? uri_size : 1))) {
        release(made);
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    if (uri_size) {
        memcpy(made->uri, uri, uri_size);
    }
    made->uri_size = uri_size;
    made->hash = hash;
    made->version = version;
    if (server->slots[at]) {
        release(server->slots[at]);
    } else {
        server->count++;
    }
    server->slots[at] = made;
    return MARKDOWN_CORE_LSP_OK;
}

markdown_core_lsp_status markdown_core_lsp_update(markdown_core_lsp_server *server, const uint8_t *uri, size_t uri_size,
                                                  int32_t version, const markdown_core_lsp_edit *edits, size_t count) {
    markdown_core_lsp_status status = active(server);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    record *held = find(server, uri, uri_size);
    if (!held) {
        return MARKDOWN_CORE_LSP_INVALID_PARAMS;
    }
    lsp_text *spare = &server->spare;
    if (!lsp_text_assign(spare, held->text.bytes, held->text.size)) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    for (size_t i = 0; i < count; i++) {
        size_t start, end;
        markdown_core_position cursor;
        lsp_text_locate(spare, edits[i].scope.start, server->unit, &start, &cursor);
        lsp_text_locate(spare, edits[i].scope.end, server->unit, &end, &cursor);
        if (start > end) {
            return MARKDOWN_CORE_LSP_INVALID_PARAMS;
        }
        if (!lsp_text_splice(spare, start, end, edits[i].text, edits[i].size)) {
            return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
        }
    }
    lsp_text edited = *spare;
    *spare = held->text;
    held->text = edited;
    held->version = version;
    markdown_core_document_free(held->document);
    held->document = NULL;
    return MARKDOWN_CORE_LSP_OK;
}

markdown_core_lsp_status markdown_core_lsp_close(markdown_core_lsp_server *server, const uint8_t *uri,
                                                 size_t uri_size) {
    markdown_core_lsp_status status = active(server);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    size_t at = probe(server, uri, uri_size, hash_uri(uri, uri_size));
    if (!server->slots[at]) {
        return MARKDOWN_CORE_LSP_INVALID_PARAMS;
    }
    release(server->slots[at]);
    vacate(server, at);
    server->count--;
    return MARKDOWN_CORE_LSP_OK;
}

markdown_core_lsp_status markdown_core_lsp_complete(markdown_core_lsp_server *server, const uint8_t *uri,
                                                    size_t uri_size, markdown_core_lsp_position position,
                                                    markdown_core_lsp_list **list) {
    markdown_core_lsp_status status = active(server);
    if (status != MARKDOWN_CORE_LSP_OK) {
        return status;
    }
    record *held = find(server, uri, uri_size);
    if (!held) {
        return MARKDOWN_CORE_LSP_INVALID_PARAMS;
    }
    if (!held->document && markdown_core_document_parse_in(held->text.bytes, held->text.size, server->unit,
                                                           &held->document) != MARKDOWN_CORE_OK) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    size_t offset;
    markdown_core_position cursor;
    lsp_text_locate(&held->text, position, server->unit, &offset, &cursor);
    /* The providers of section 5 read this context; until they land, every
     * slot has no candidates. */
    markdown_core_context *context;
    if (markdown_core_document_context(held->document, held->text.bytes, held->text.size, cursor, &context) !=
        MARKDOWN_CORE_OK) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    markdown_core_context_free(context);
    markdown_core_lsp_list *made = (markdown_core_lsp_list *)calloc(1, sizeof(*made));
    if (!made) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    made->version = held->version;
    made->count = 0;
    *list = made;
    return MARKDOWN_CORE_LSP_OK;
}

const lsp_text *lsp_server_text(const markdown_core_lsp_server *server, const uint8_t *uri, size_t uri_size) {
    const record *held = find(server, uri, uri_size);
    return held ? &held->text : NULL;
}

int32_t markdown_core_lsp_list_version(const markdown_core_lsp_list *list) { return list->version; }

size_t markdown_core_lsp_list_count(const markdown_core_lsp_list *list) { return list->count; }

void markdown_core_lsp_list_free(markdown_core_lsp_list *list) { free(list); }
