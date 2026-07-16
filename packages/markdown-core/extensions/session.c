#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session_internal.h"

#include "directive.h"

#include <iterator.h>
#include <node.h>

// A session is a purely local object: it owns its text, its committed tree,
// and its id table, and shares no state with any other session or any global.
// Every commit currently performs a full staged reparse of the stored text;
// the incremental damage/resync machinery replaces the staging step later
// without changing this API or the id/changeset semantics.

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

// Mirrors set_error in ast.c; sessions and documents share the error type but
// not a translation unit private to either.
void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message);

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// --- id table ---------------------------------------------------------------

static void id_table_release(markdown_core_mem *mem, markdown_core_id_table *table) {
    if (table->keys) {
        mem->free(table->keys);
    }
    if (table->values) {
        mem->free(table->values);
    }
    table->keys = NULL;
    table->values = NULL;
    table->capacity = 0;
    table->count = 0;
}

static bool id_table_insert(markdown_core_id_table *table, markdown_core_node_id id, markdown_core_node *node) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)mix64(id) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == id) {
            table->values[slot] = node;
            return true;
        }
        slot = (slot + 1) & mask;
    }
    table->keys[slot] = id;
    table->values[slot] = node;
    table->count++;
    return true;
}

// Builds a fresh id table for `root` into `out` (owned by the caller on
// success). Runs inside mutating calls only, so concurrent readers never
// observe a table under construction.
static bool id_table_build(markdown_core_mem *mem, markdown_core_node *root, markdown_core_id_table *out) {
    size_t nodes = 0;

    markdown_core_iter *iter = markdown_core_iter_new(root);
    if (!iter) {
        return false;
    }
    markdown_core_event_type ev;
    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (ev == MARKDOWN_CORE_EVENT_ENTER) {
            nodes++;
        }
    }
    markdown_core_iter_free(iter);

    size_t capacity = 16;
    while (capacity < nodes * 2) {
        capacity *= 2;
    }

    out->keys = (markdown_core_node_id *)mem->calloc(capacity, sizeof(markdown_core_node_id));
    out->values = (markdown_core_node **)mem->calloc(capacity, sizeof(markdown_core_node *));
    out->count = 0;
    if (!out->keys || !out->values) {
        id_table_release(mem, out);
        return false;
    }
    out->capacity = capacity;

    iter = markdown_core_iter_new(root);
    if (!iter) {
        id_table_release(mem, out);
        return false;
    }
    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (ev != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        // Directive-label wrappers are facade-invisible and not addressable.
        if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
            continue;
        }
        id_table_insert(out, node->id, node);
    }
    markdown_core_iter_free(iter);
    return true;
}

// --- parsing ----------------------------------------------------------------

static int native_options_from(const markdown_core_parse_options *options) {
    int native_options = MARKDOWN_CORE_OPT_VALIDATE_UTF8;
    if (options->smart_punctuation) {
        native_options |= MARKDOWN_CORE_OPT_SMART;
    }
    if (options->footnotes) {
        native_options |= MARKDOWN_CORE_OPT_FOOTNOTES;
    }
    if (options->strip_html_comments) {
        native_options |= MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
    }
    if (options->formulas && options->dollar_formula_delimiters) {
        native_options |= MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS;
    }
    if (options->formulas && options->latex_formula_delimiters) {
        native_options |= MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
    }
    if (options->directives) {
        native_options |= MARKDOWN_CORE_OPT_DIRECTIVE;
    }
    return native_options;
}

static bool attach_extension_named(markdown_core_parser *parser, const char *name) {
    markdown_core_extension *extension = markdown_core_extension_find(name);
    return extension && markdown_core_parser_attach_extension(parser, extension) != 0;
}

// Seals a freshly parsed tree: line positions convert from absolute to
// parent-relative deltas (columns are line-local already) and every node
// gains MARKDOWN_CORE_NODE__SEALED_RELATIVE. Post-order, so each conversion
// still reads its parent's absolute start; pointer-walk iterative, because
// adversarial inputs nest too deep for native recursion.
static void seal_positions(markdown_core_node *root) {
    markdown_core_node *node = root;
    for (;;) {
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            int start_line = node->start_line;
            if (node->parent) {
                node->start_line = start_line - node->parent->start_line;
            }
            node->end_line -= start_line;
            node->flags |= MARKDOWN_CORE_NODE__SEALED_RELATIVE;
            if (node == root) {
                return;
            }
            if (node->next) {
                node = node->next;
                break;
            }
            node = node->parent;
        }
    }
}

// Parses the session's current text from scratch. Returns NULL with *error
// set on failure; the session state is untouched.
static markdown_core_node *parse_text(markdown_core_session *session, markdown_core_error **error) {
    markdown_core_parser *parser =
        markdown_core_parser_new_with_mem(native_options_from(&session->options), session->mem);
    if (!parser) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

    const markdown_core_parse_options *options = &session->options;
    bool attached = (!options->tables || attach_extension_named(parser, "table")) &&
                    (!options->strikethrough || attach_extension_named(parser, "strikethrough")) &&
                    (!options->autolinks || attach_extension_named(parser, "autolink")) &&
                    (!options->task_lists || attach_extension_named(parser, "tasklist")) &&
                    (!options->formulas || attach_extension_named(parser, "formula")) &&
                    (!options->directives || attach_extension_named(parser, "directive"));
    if (!attached) {
        markdown_core_parser_free(parser);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "required syntax extension is unavailable");
        return NULL;
    }

    size_t length = markdown_core_text_length(&session->text);
    if (length) {
        markdown_core_parser_feed(parser, (const char *)markdown_core_text_bytes(&session->text), length);
    }
    markdown_core_node *root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!root) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "parser did not produce a document");
        return NULL;
    }
    seal_positions(root);
    return root;
}

static bool commit_internal(markdown_core_session *session, bool initial, markdown_core_changeset **changes_out,
                            markdown_core_error **error) {
    markdown_core_changeset *changes = NULL;

    markdown_core_node *root = parse_text(session, error);
    if (!root) {
        return false;
    }

    uint64_t new_rev = initial ? 0 : session->revision + 1;

    if (changes_out) {
        changes = (markdown_core_changeset *)calloc(1, sizeof(*changes));
        if (!changes) {
            markdown_core_node_free(root);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate changeset");
            return false;
        }
        changes->before = session->revision;
        changes->after = new_rev;
    }

    if (!markdown_core_session_adopt(session, session->view.root, root, new_rev, changes)) {
        markdown_core_changeset_free(changes);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not record the changeset");
        return false;
    }

    // The lookup table is maintained here, inside the mutating call, so
    // markdown_core_session_node_by_id stays a pure concurrent-safe read.
    markdown_core_id_table ids = {NULL, NULL, 0, 0};
    if (!id_table_build(session->mem, root, &ids)) {
        markdown_core_changeset_free(changes);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                    "could not index the committed document");
        return false;
    }

    if (session->view.root) {
        markdown_core_node_free(session->view.root);
    }
    id_table_release(session->mem, &session->ids);
    session->view.root = root;
    session->ids = ids;
    session->revision = new_rev;

    if (changes_out) {
        *changes_out = changes;
    }
    return true;
}

// --- public API -------------------------------------------------------------

markdown_core_session *markdown_core_session_open_with_mem(const markdown_core_parse_options *options,
                                                           markdown_core_mem *mem, markdown_core_error **error) {
    clear_error(error);

    markdown_core_session *session = (markdown_core_session *)calloc(1, sizeof(*session));
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
        return NULL;
    }

    if (options) {
        session->options = *options;
    } else {
        markdown_core_parse_options_init(&session->options);
    }
    session->mem = mem;
    markdown_core_text_init(&session->text, mem);
    session->next_id = 1;
    session->revision = 0;

    // Purely local entropy: no global RNG state. The lineage only has to make
    // accidental cross-session id equality vanishingly unlikely.
    uint64_t entropy = (uint64_t)(uintptr_t)session;
    entropy ^= mix64((uint64_t)time(NULL));
    entropy ^= mix64((uint64_t)clock()) << 1;
    session->lineage = mix64(entropy);

    if (!commit_internal(session, true, NULL, error)) {
        markdown_core_session_free(session);
        return NULL;
    }
    return session;
}

markdown_core_session *markdown_core_session_open(const markdown_core_parse_options *options,
                                                  markdown_core_error **error) {
    return markdown_core_session_open_with_mem(options, markdown_core_mem_default(), error);
}

void markdown_core_session_free(markdown_core_session *session) {
    if (!session) {
        return;
    }
    if (session->view.root) {
        markdown_core_node_free(session->view.root);
    }
    markdown_core_text_release(&session->text);
    id_table_release(session->mem, &session->ids);
    free(session);
}

bool markdown_core_session_edit(markdown_core_session *session, size_t byte_start, size_t byte_end,
                                const uint8_t *bytes, size_t length, markdown_core_error **error) {
    clear_error(error);
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "session must not be null");
        return false;
    }
    if (byte_start > byte_end || byte_end > markdown_core_text_length(&session->text)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
                                    "edit range is outside the current text");
        return false;
    }
    if (!bytes && length != 0) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
                                    "bytes must not be null when length is nonzero");
        return false;
    }
    if (!markdown_core_text_edit(&session->text, byte_start, byte_end, bytes, length)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not apply the edit");
        return false;
    }
    return true;
}

bool markdown_core_session_commit(markdown_core_session *session, markdown_core_changeset **changes,
                                  markdown_core_error **error) {
    clear_error(error);
    if (changes) {
        *changes = NULL;
    }
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "session must not be null");
        return false;
    }
    return commit_internal(session, false, changes, error);
}

const markdown_core_document *markdown_core_session_document(const markdown_core_session *session) {
    return session ? &session->view : NULL;
}

uint64_t markdown_core_session_revision(const markdown_core_session *session) {
    return session ? session->revision : 0;
}

uint64_t markdown_core_session_lineage(const markdown_core_session *session) { return session ? session->lineage : 0; }

size_t markdown_core_session_length(const markdown_core_session *session) {
    return session ? markdown_core_text_length(&session->text) : 0;
}

const markdown_core_node *markdown_core_session_node_by_id(const markdown_core_session *session,
                                                           markdown_core_node_id id) {
    if (!session || id == 0) {
        return NULL;
    }
    if (session->ids.capacity == 0) {
        return NULL;
    }
    size_t mask = session->ids.capacity - 1;
    size_t slot = (size_t)mix64(id) & mask;
    while (session->ids.keys[slot] != 0) {
        if (session->ids.keys[slot] == id) {
            return session->ids.values[slot];
        }
        slot = (slot + 1) & mask;
    }
    return NULL;
}
