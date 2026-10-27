#ifndef MARKDOWN_CORE_LSP_ADAPTER_H
#define MARKDOWN_CORE_LSP_ADAPTER_H

#include "json.h"
#include "markdown_core_lsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The JSON-RPC adapter (plan, section 4.2): one LSP 3.17 message in, at
 * most one message out, mapped onto the engine's calls. It holds no state
 * the engine does not hold; the tree, the edits and the answer are scratch
 * that each message reuses.
 */

typedef struct adapter {
    markdown_core_lsp_server *server;
    json_tree tree;
    markdown_core_lsp_edit *edits;
    size_t room;
    /* The answer to the last message, empty when it has none. */
    json_out out;
    /* Whether the last message was the exit notification. */
    bool exited;
} adapter;

/* False when the server cannot be allocated. */
bool adapter_open(adapter *self);
void adapter_close(adapter *self);

/* Handles the message of `size` bytes at `body`. Afterwards `out` holds the
 * answer, if any, and `out.failed` says it could not be written. */
void adapter_handle(adapter *self, const uint8_t *body, size_t size);

/* The process's exit code after `exit` or the end of the input: 0 when the
 * server was shut down first, 1 otherwise. */
int adapter_code(const adapter *self);

#endif
