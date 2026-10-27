#ifndef MARKDOWN_CORE_LSP_H
#define MARKDOWN_CORE_LSP_H

#include "markdown_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The language engine (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 4): the semantics of the Language Server Protocol 3.17 over
 * Markdown Core, as a library a host calls in process. The JSON-RPC adapter
 * `markdown-core-server` maps the protocol's messages onto these calls, and
 * each platform binding wraps them, so every host gets the same answers.
 *
 * A SERVER holds the open documents of one client. The engine does no I/O:
 * the host hands it every text. One thread at a time may call a server; the
 * results it returns are immutable and may be read on any thread, and each
 * lives until its own `_free`.
 *
 * Errors: a call that can fail answers a markdown_core_lsp_status, whose
 * value is the protocol's error code, and writes its out-parameters only
 * when it answers MARKDOWN_CORE_LSP_OK. A host answers a request that fails
 * with that code and drops a notification that fails, as the protocol does.
 * A failed call changes nothing. Pointer arguments are not checked for NULL,
 * except that a text of size 0 may be NULL and `*_free(NULL)` does nothing.
 *
 * Positions are the protocol's: a line and a character offset counted from
 * 0, in the unit `markdown_core_lsp_initialize` negotiated. A line ends at
 * `\n`, `\r\n` or `\r`. A character offset past its line's end stands at the
 * line's end, one inside a character stands at the character's start, and a
 * line past the text's last stands at the text's end.
 */

#if defined(_WIN32) && !defined(MARKDOWN_CORE_LSP_STATIC_DEFINE)
#if defined(MARKDOWN_CORE_LSP_EXPORTS)
#define MARKDOWN_CORE_LSP_API __declspec(dllexport)
#else
#define MARKDOWN_CORE_LSP_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MARKDOWN_CORE_LSP_API __attribute__((visibility("default")))
#else
#define MARKDOWN_CORE_LSP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct markdown_core_lsp_server markdown_core_lsp_server;

/** What a call that can fail answers: MARKDOWN_CORE_LSP_OK, or the protocol's
 * error code for the one reason there is no answer.
 *
 * - SERVER_NOT_INITIALIZED: the call came before initialize.
 * - INVALID_REQUEST: the call is not one the server's phase takes: a second
 *   initialize, or any call after shutdown.
 * - INVALID_PARAMS: the call names a document that is not open, or an edit
 *   whose start is past its end.
 * - INTERNAL_ERROR: an allocation failed, or a text exceeds what the parser
 *   can hold. */
typedef enum markdown_core_lsp_status {
    MARKDOWN_CORE_LSP_OK = 0,
    MARKDOWN_CORE_LSP_SERVER_NOT_INITIALIZED = -32002,
    MARKDOWN_CORE_LSP_INVALID_REQUEST = -32600,
    MARKDOWN_CORE_LSP_INVALID_PARAMS = -32602,
    MARKDOWN_CORE_LSP_INTERNAL_ERROR = -32603
} markdown_core_lsp_status;

/** Where a server is in the protocol's lifecycle: FRESH until initialize,
 * ACTIVE until shutdown, and HALTED afterwards. A host that reads `exit`
 * ends with code 0 when the server is HALTED, and 1 otherwise. */
typedef enum markdown_core_lsp_phase {
    MARKDOWN_CORE_LSP_FRESH = 0,
    MARKDOWN_CORE_LSP_ACTIVE = 1,
    MARKDOWN_CORE_LSP_HALTED = 2
} markdown_core_lsp_phase;

/** A protocol position: `line` and `character` counted from 0. */
typedef struct markdown_core_lsp_position {
    uint32_t line;
    uint32_t character;
} markdown_core_lsp_position;

/** A protocol range: from `start` up to `end`, which it does not hold. */
typedef struct markdown_core_lsp_scope {
    markdown_core_lsp_position start;
    markdown_core_lsp_position end;
} markdown_core_lsp_scope;

/** An edit: `size` bytes of UTF-8 at `text` that replace `scope`. A content
 * change that names no range replaces the whole text, which is the scope
 * from 0:0 to UINT32_MAX:UINT32_MAX. */
typedef struct markdown_core_lsp_edit {
    markdown_core_lsp_scope scope;
    const uint8_t *text;
    size_t size;
} markdown_core_lsp_edit;

/** What a server answers initialize with.
 *
 * - `unit` is the position encoding: UTF-8 when the client offers it, and
 *   UTF-16 otherwise, which every client supports.
 * - `cues` are the completion trigger characters, one per byte: the
 *   parser's slot cues (markdown_core_slot_cues). They borrow from the
 *   library.
 *
 * The server always takes open and close notifications and incremental
 * changes (TextDocumentSyncKind.Incremental). */
typedef struct markdown_core_lsp_capabilities {
    markdown_core_text_unit unit;
    markdown_core_string cues;
} markdown_core_lsp_capabilities;

/** A completion list: the answer to one completion request, computed for
 * one version of its document. A host drops a list whose version is older
 * than the document's, as the protocol's ContentModified does. */
typedef struct markdown_core_lsp_list markdown_core_lsp_list;

/** `*server` receives a new server, in the FRESH phase, which
 * markdown_core_lsp_server_free releases with every document it holds.
 * INTERNAL_ERROR when it cannot be allocated. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_server_new(markdown_core_lsp_server **server);
MARKDOWN_CORE_LSP_API void markdown_core_lsp_server_free(markdown_core_lsp_server *server);
MARKDOWN_CORE_LSP_API markdown_core_lsp_phase markdown_core_lsp_server_phase(const markdown_core_lsp_server *server);

/** The initialize request. `units` are the `count` position encodings the
 * client offers, which the protocol's `general.positionEncodings` lists;
 * an encoding the parser does not count in is left out. The server becomes
 * ACTIVE. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status
markdown_core_lsp_initialize(markdown_core_lsp_server *server, const markdown_core_text_unit *units, size_t count,
                             markdown_core_lsp_capabilities *capabilities);
/** The shutdown request. The server becomes HALTED and takes no more calls. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_shutdown(markdown_core_lsp_server *server);

/** The didOpen notification: the document at `uri`, `uri_size` bytes
 * compared as they are, holds `text_size` bytes of `text` at `version`. A
 * document opened again replaces the one open at its URI. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_open(markdown_core_lsp_server *server,
                                                                      const uint8_t *uri, size_t uri_size,
                                                                      int32_t version, const uint8_t *text,
                                                                      size_t text_size);
/** The didChange notification: `count` edits applied to the document in
 * order, each to the text the one before it made, which give it `version`.
 * INVALID_PARAMS when an edit's start is past its end, and then the
 * document keeps its text and version. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_update(markdown_core_lsp_server *server,
                                                                        const uint8_t *uri, size_t uri_size,
                                                                        int32_t version,
                                                                        const markdown_core_lsp_edit *edits,
                                                                        size_t count);
/** The didClose notification. */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_close(markdown_core_lsp_server *server,
                                                                       const uint8_t *uri, size_t uri_size);

/** The completion request at `position` in the document at `uri`. `*list`
 * receives a new list, which markdown_core_lsp_list_free releases. It
 * parses the document's version once, on the first request that needs it,
 * and reads the parser's context at the cursor (plan, section 6). */
MARKDOWN_CORE_LSP_API markdown_core_lsp_status markdown_core_lsp_complete(markdown_core_lsp_server *server,
                                                                          const uint8_t *uri, size_t uri_size,
                                                                          markdown_core_lsp_position position,
                                                                          markdown_core_lsp_list **list);
/** The version of the document the list was computed for. */
MARKDOWN_CORE_LSP_API int32_t markdown_core_lsp_list_version(const markdown_core_lsp_list *list);
/** The number of items in the list. */
MARKDOWN_CORE_LSP_API size_t markdown_core_lsp_list_count(const markdown_core_lsp_list *list);
MARKDOWN_CORE_LSP_API void markdown_core_lsp_list_free(markdown_core_lsp_list *list);

#ifdef __cplusplus
}
#endif

#endif
