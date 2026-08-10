#if defined(_WIN32) && !defined(_CRT_RAND_S)
// rand_s is the linkage-free CSPRNG on Windows and must be requested before
// the first stdlib.h include.
#define _CRT_RAND_S
#endif
#if (defined(__EMSCRIPTEN__) || defined(__wasi__)) && !defined(_GNU_SOURCE)
// musl only declares getentropy outside strict-standard mode.
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
#include <unistd.h>
#elif !defined(__APPLE__) && !defined(_WIN32)
#include <stdio.h>
#endif

#include "document_internal.h"

#include <iterator.h>
#include <node.h>
#include <parser.h>

// A session is a purely local object: it owns its text, its committed tree,
// its reference map, and its id table, and shares no state with any other
// session or any global. Commits route through the incremental pipeline in
// incremental.c when the edits allow it and fall back to a full staged reparse
// (this file) otherwise; both produce identical observable results, which the
// equivalence suite enforces.

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

// --- parsing ----------------------------------------------------------------

static int native_options_from(const markdown_core_parse_options *options) {
    /* UTF-8 is assumed and never validated (7.1); nothing is switched on
     * here to check it. */
    int native_options = 0;
    if (options->smart_punctuation) {
        native_options |= MARKDOWN_CORE_OPT_SMART;
    }
    if (options->footnotes) {
        native_options |= MARKDOWN_CORE_OPT_FOOTNOTES;
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

markdown_core_parser *markdown_core_document_new_parser(markdown_core_document *session, markdown_core_error **error) {
    markdown_core_parser *parser =
        markdown_core_parser_new_with_mem(native_options_from(&session->options), session->mem);
    if (!parser) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

    const markdown_core_parse_options *options = &session->options;
    /* Attachment order is priority. `table` is attached last because its row
     * opener accepts any non-blank line inside an open table, so anything with
     * a narrower claim has to be offered the line first — which is exactly
     * what the GFM specification requires of a table: "The table is broken at
     * the first empty line, or beginning of another block-level structure."
     * A construct that matches everything can only ever be the fallback. */
    bool attached = (!options->strikethrough || attach_extension_named(parser, "strikethrough")) &&
                    (!options->autolinks || attach_extension_named(parser, "autolink")) &&
                    (!options->task_lists || attach_extension_named(parser, "tasklist")) &&
                    (!options->formulas || attach_extension_named(parser, "formula")) &&
                    (!options->directives || attach_extension_named(parser, "directive")) &&
                    (!options->cross_links || attach_extension_named(parser, "cross_link")) &&
                    (!options->embeds || attach_extension_named(parser, "embed")) &&
                    (!options->tables || attach_extension_named(parser, "table"));
    if (!attached) {
        bool allocation_failed = parser->oom;
        markdown_core_parser_free(parser);
        markdown_core_ast_set_error(
            error,
            allocation_failed ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
            allocation_failed ? "could not attach the required syntax extensions"
                              : "required syntax extension is unavailable"
        );
        return NULL;
    }
    return parser;
}

markdown_core_parser *markdown_core_document_acquire_parser(
    markdown_core_document *session,
    markdown_core_error **error
) {
    markdown_core_parser *parser = session->warm_parser;
    if (parser) {
        session->warm_parser = NULL;
        return parser;
    }
    return markdown_core_document_new_parser(session, error);
}

void markdown_core_document_release_parser(markdown_core_document *session, markdown_core_parser *parser) {
    if (!parser) {
        return;
    }
    if (!parser->oom && !parser->internal_error && !session->warm_parser) {
        markdown_core_parser_renew(parser);
        if (!parser->oom && !parser->internal_error) {
            session->warm_parser = parser;
            return;
        }
    }
    markdown_core_parser_free(parser);
}

void markdown_core_document_resolve_definition_owners(markdown_core_map *map) {
    markdown_core_map_entry *entry;
    for (entry = map->refs; entry; entry = entry->next) {
        if (entry->owner != 0) {
            entry->owner = ((const markdown_core_node *)(uintptr_t)entry->owner)->id;
        }
        /* Unguarded, unlike `owner`: 0 is a real owner (the region before the
         * first document child), while every entry that reaches adoption was
         * stamped with the node it was written as. A definition whose node
         * was lost to allocation failure fails the parse instead, so its
         * entry never gets here. */
        entry->definition_node = ((const markdown_core_node *)(uintptr_t)entry->definition_node)->id;
    }
}

/* Frees a set of definition tables — the staged ones a failed full commit
 * abandons, or the committed ones it replaces. */
static void release_definition_tables(markdown_core_mem *mem, markdown_core_definition_table *tables) {
    size_t s;
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_map_free(tables[s].map);
        if (tables[s].index) {
            mem->free(mem, tables[s].index);
        }
        memset(&tables[s], 0, sizeof(tables[s]));
    }
}

// Full staged reparse: parses the whole stored text with a fresh parser and
// fresh definition maps, adopts ids from the previous tree, and replaces every
// piece of session state at once. The staging never touches the committed
// state, so any failure leaves the session valid at its previous revision.
/* PARSE. A pure function of (bytes, options): it fills this document's tree,
 * definition maps and footnote index from its own stored text and reads
 * nothing else. No predecessor, no ids — identity is the diff's to assign. */
static bool document_parse_text(markdown_core_document *session, markdown_core_error **error) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    markdown_core_definition_table staged[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    int total_lines;
    int last_line_length;
    size_t s;

    parser = markdown_core_document_acquire_parser(session, error);
    if (!parser) {
        return false;
    }


    // Fed run by run. markdown_core_parser_feed is a streaming interface
    // (core/blocks.c S_parser_feed buffers a partial line in parser->linebuf),
    // so the chunking is free to follow whatever the store hands back — which,
    // now that the store is one flat buffer, is the whole document at once.
    size_t length = markdown_core_source_length(session->source);
    {
        size_t pos = 0;
        while (pos < length) {
            size_t run = 0;
            const uint8_t *bytes = markdown_core_source_run_at(session->source, pos, &run);
            markdown_core_parser_feed(parser, (const char *)bytes, run);
            pos += run;
        }
    }
    markdown_core_parser_finalize_blocks(parser);
    total_lines = parser->line_number;
    last_line_length = parser->last_line_length;
    root = markdown_core_parser_refine_blocks(parser);
    if (!root) {
        bool allocation_failed = parser->oom;
        bool internal_error = parser->internal_error;
        markdown_core_parser_free(parser); // frees the staged maps with it
        markdown_core_ast_set_error(
            error,
            allocation_failed || !internal_error ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
            allocation_failed || !internal_error ? "could not parse the session text"
                                                 : "parser refinement invariant failed"
        );
        return false;
    }
    memset(staged, 0, sizeof(staged));
    staged[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map = parser->refmap;
    staged[MARKDOWN_CORE_DEFINITIONS_FOOTNOTES].map = parser->footnote_defs;
    parser->refmap = NULL;
    parser->footnote_defs = NULL;
    markdown_core_document_release_parser(session, parser);
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        // The sink's context is this call's stack frame; the maps outlive it.
        // Both are present: a parse that lost one is poisoned and returns no
        // root, so this line is only reached with the tree in hand.
        staged[s].map->lookup_sink = NULL;
        staged[s].map->lookup_context = NULL;
        staged[s].map->lookup_unit = NULL;
    }
    if (false) {
        release_definition_tables(session->mem, staged);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not record the document's reference lookups"
        );
        return false;
    }


    memcpy(session->definitions, staged, sizeof(staged));
    session->root = root;
    session->total_lines = total_lines;
    session->last_line_length = last_line_length;
    return true;
}

/* DIFF. `new` has a tree and no identities; `old` may be NULL. This assigns
 * `new`'s identities from `old` — the one decision both requirements rest on
 * — and reports what changed. It reads no text and reparses nothing.
 *
 * A diff of the same two trees is the same diff. That is the invariant the
 * old `adopt` name hid, and keeping parse and diff apart is what makes it
 * statable: a tree is a pure function of (bytes, options), and a delta is a
 * pure function of two trees. */
bool markdown_core_document_diff(
    const markdown_core_document *old,
    markdown_core_document *nw,
    markdown_core_delta *changes,
    markdown_core_error **error
) {
    markdown_core_footnote_index footnotes;
    size_t s;

    if (!markdown_core_diff_trees(nw, old ? old->root : NULL, nw->root, nw->revision, changes)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not record the delta");
        return false;
    }
    // Ids exist now, so definitions recorded against anchor node pointers can
    // take their ids.
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_document_resolve_definition_owners(nw->definitions[s].map);
    }

    // Footnote numbering, resolution and back-reference ordinals are
    // index-backed answers, and the delta says NOTHING about them: the
    // ANSWERS part is gone, and so is the second pass that existed to bump
    // revisions when only a number moved. Nothing shipped ever read those
    // answers -- no binding calls footnote_info, footnotes,
    // footnote_references or reference_info -- so there was no consumer to
    // under-report to, and reporting them cost a whole extra emission pass.
    memset(&footnotes, 0, sizeof(footnotes));
    if (nw->options.footnotes) {
        if (!markdown_core_footnote_index_build(nw, nw->root, &footnotes)) {
            markdown_core_footnote_index_release(nw->mem, &footnotes);
            markdown_core_ast_set_error(
                error,
                MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                "could not index the document's footnotes"
            );
            return false;
        }
    }

    nw->footnotes = footnotes;
    return true;
}

static markdown_core_document *markdown_core_document_alloc(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
);

/* Replaces the document's whole text. The source starts empty, so this is one
 * insertion at the origin. */
static bool document_set_text(
    markdown_core_document *doc,
    markdown_core_string markdown,
    markdown_core_error **error
) {
    markdown_core_source_edit edit;
    markdown_core_source_stats stats;
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    memset(&stats, 0, sizeof(stats));
    edit.span.start = 0;
    edit.span.end = 0;
    edit.replacement = markdown.data;
    edit.replacement_length = markdown.length;
    if (!markdown_core_source_apply(doc->source, &edit, 1, &stats, &status)) {
        markdown_core_ast_set_error(
            error,
            status == MARKDOWN_CORE_SOURCE_NO_MEMORY ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED
                                                     : MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "could not store the document text"
        );
        return false;
    }
    return true;
}

/* BUILD ONE DOCUMENT FROM TEXT, and diff it against `prev` if there is one.
 *
 *     new   = Document(markdown, options)
 *     delta = diff(prev, new)
 *
 * That is the whole of a commit. There is no incremental path, no pending
 * edit, no reuse of the previous tree: the previous document is INPUT to the
 * diff and nothing else, and it is untouched by this call. */
static markdown_core_document *document_build(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
    const markdown_core_document *prev,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_delta **changes_out,
    markdown_core_error **error
) {
    markdown_core_document *doc;
    markdown_core_delta *changes = NULL;

    doc = markdown_core_document_alloc(options, mem, pooled, error);
    if (!doc) {
        return NULL;
    }
    if (prev) {
        doc->lineage = prev->lineage;
        doc->next_id = prev->next_id;
        doc->revision = prev->revision + 1;
    }
    if (markdown.length && !document_set_text(doc, markdown, error)) {
        markdown_core_document_release(doc);
        return NULL;
    }
    if (changes_out) {
        changes = (markdown_core_delta *)calloc(1, sizeof(*changes));
        if (!changes) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate delta");
            markdown_core_document_release(doc);
            return NULL;
        }
        changes->lineage = doc->lineage;
        changes->before = prev ? prev->revision : 0;
        changes->after = doc->revision;
    }
    if (!document_parse_text(doc, error) || !markdown_core_document_diff(prev, doc, changes, error)) {
        markdown_core_delta_free(changes);
        markdown_core_document_release(doc);
        return NULL;
    }
    if (changes_out) {
        *changes_out = changes;
    }
    return doc;
}

// One 64-bit read from the host CSPRNG. Sessions stay free of any library-
// owned RNG state: every source below is the platform's own, shared-nothing
// entropy service.
static bool session_host_entropy(uint64_t *value) {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    // Standalone WASM lowers getentropy to the WASI random_get import; hosts
    // without the import report failure here instead of trapping.
    return getentropy(value, sizeof(*value)) == 0;
#elif defined(__APPLE__)
    arc4random_buf(value, sizeof(*value));
    return true;
#elif defined(_WIN32)
    unsigned int low = 0;
    unsigned int high = 0;
    if (rand_s(&low) != 0 || rand_s(&high) != 0) {
        return false;
    }
    *value = ((uint64_t)high << 32) | (uint64_t)low;
    return true;
#else
    FILE *source = fopen("/dev/urandom", "rb");
    bool complete = false;
    if (source) {
        complete = fread(value, sizeof(*value), 1, source) == 1;
        fclose(source);
    }
    return complete;
#endif
}

// --- public API -------------------------------------------------------------

/* Allocates a document and its empty source. No parse; document_build does
 * that once, with the text in hand. */
static markdown_core_document *markdown_core_document_alloc(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    clear_error(error);

    markdown_core_document *session = (markdown_core_document *)calloc(1, sizeof(*session));
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
        return NULL;
    }

    if (options) {
        session->options = *options;
    } else {
        markdown_core_parse_options_init(&session->options);
    }
#if MARKDOWN_CORE_ASAN
    // Slab-carved and freelist-reused blocks are invisible to
    // AddressSanitizer, so pooling would blind the ASan suites to
    // use-after-free and overflow inside session memory. The sanitizer
    // build exercises the exact same allocation paths against the base
    // allocator instead.
    pooled = false;
#endif
    if (pooled) {
        session->arena = markdown_core_arena_new(mem);
        if (!session->arena) {
            free(session);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
            return NULL;
        }
        mem = markdown_core_arena_mem(session->arena);
    }
    session->mem = mem;
    {
        // The store holds whatever bytes it is handed. UTF-8 is assumed and
        // never validated (7.1), and a streamed append completes a multi-byte
        // character whose first bytes arrived earlier — deciding that at the
        // substrate was always the wrong layer, and there is no longer a
        // profile that could.
        markdown_core_source_stats scratch;
        markdown_core_source_status status;
        memset(&scratch, 0, sizeof(scratch));
        session->source = markdown_core_source_new(mem, NULL, 0, &scratch, &status);
        if (!session->source) {
            // Unwound here rather than through markdown_core_document_release:
            // that path releases session->source unconditionally, and it is
            // the thing that just failed to exist.
            if (session->arena) {
                markdown_core_arena_release(session->arena);
            }
            free(session);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
            return NULL;
        }
    }
    session->next_id = 1;
    session->revision = 0;

    // The address/time/clock mix alone is deterministic for the first
    // session of lockstep-started isolated runtimes (one WASM instance per
    // worker reproduces the same allocator state and coarse clocks), so the
    // host CSPRNG carries the cross-runtime uniqueness contract. The local
    // mix stays folded in as a best-effort fallback when the host read
    // fails.
    uint64_t entropy = (uint64_t)(uintptr_t)session;
    uint64_t host_entropy = 0;
    entropy ^= markdown_core_mix64((uint64_t)time(NULL));
    entropy ^= markdown_core_mix64((uint64_t)clock()) << 1;
    if (session_host_entropy(&host_entropy)) {
        entropy ^= host_entropy;
    }
    session->lineage = markdown_core_mix64(entropy);
    return session;
}

markdown_core_document *markdown_core_document_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    markdown_core_string empty = {NULL, 0};
    return document_build(options, empty, NULL, mem, pooled, NULL, error);
}

/* `Document(markdown, options)` — the one entry point. */
markdown_core_document *markdown_core_document_new(
    markdown_core_string markdown,
    const markdown_core_parse_options *options,
    markdown_core_error **error
) {
    clear_error(error);
    if (!markdown.data && markdown.length != 0) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "markdown must not be null when length is nonzero"
        );
        return NULL;
    }
    return document_build(options, markdown, NULL, markdown_core_mem_default(), true, NULL, error);
}

markdown_core_document *markdown_core_document_open(
    const markdown_core_parse_options *options,
    markdown_core_error **error
) {
    return markdown_core_document_open_with_mem(options, markdown_core_mem_default(), true, error);
}

void markdown_core_document_release(markdown_core_document *session) {
    if (!session) {
        return;
    }
    if (session->arena) {
        // Everything session-owned came from the arena (deltas and errors
        // are caller-owned system allocations); one release replaces the
        // per-structure teardown below.
        markdown_core_arena_release(session->arena);
        free(session);
        return;
    }
    if (session->root) {
        markdown_core_node_free(session->root);
    }
    markdown_core_source_release(session->source);
    markdown_core_footnote_index_release(session->mem, &session->footnotes);
    release_definition_tables(session->mem, session->definitions);
    markdown_core_footnote_labels_release(session->mem, &session->footnote_labels);
    if (session->warm_parser) {
        markdown_core_parser_free(session->warm_parser);
    }
    free(session);
}

bool markdown_core_document_splice(
    markdown_core_document *session,
    size_t byte_start,
    size_t byte_end,
    const uint8_t *bytes,
    size_t length,
    markdown_core_error **error
) {
    clear_error(error);
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "session must not be null");
        return false;
    }
    if (byte_start > byte_end || byte_end > markdown_core_source_length(session->source)) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "edit range is outside the current text"
        );
        return false;
    }
    if (!bytes && length != 0) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "bytes must not be null when length is nonzero"
        );
        return false;
    }
    {
        // One edit per call, because that is what the convenience API takes.
        // The substrate accepts a batch, so a caller with several edits
        // splices once for all of them; a keystroke splices for itself.
        markdown_core_source_edit edit;
        markdown_core_source_stats stats;
        markdown_core_source_status status;

        edit.span.start = byte_start;
        edit.span.end = byte_end;
        edit.replacement = bytes;
        edit.replacement_length = length;
        memset(&stats, 0, sizeof(stats));
        if (!markdown_core_source_apply(session->source, &edit, 1, &stats, &status)) {
            // The span was validated above, so the substrate's remaining
            // refusals are all "this edit cannot be stored" — a failed
            // allocation or a length size_t cannot represent. Both are the
            // allocation failure this call has always reported, and both
            // leave the source untouched.
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not apply the edit");
            return false;
        }
    }

    // Coalesce into the pending summary. The stored range lives in
    // current-text coordinates, so first map the existing range through this
    // edit, then union in the freshly written bytes.
    {
        markdown_core_edit_summary *pending = &session->pending;
        size_t removed = byte_end - byte_start;
        if (pending->dirty) {
            size_t hi = pending->new_hi;
            if (hi > byte_start) {
                hi = hi <= byte_end ? byte_start + length : hi + length - removed;
            }
            if (pending->new_lo > byte_start) {
                pending->new_lo = byte_start;
            }
            pending->new_hi = hi > byte_start + length ? hi : byte_start + length;
        } else {
            pending->dirty = true;
            pending->new_lo = byte_start;
            pending->new_hi = byte_start + length;
        }
        pending->delta += (ptrdiff_t)length - (ptrdiff_t)removed;
    }
    return true;
}

/* EDIT: hand the document new text.
 *
 *     let new   = Document(markdown, document.options)
 *     let delta = diff(document, new)
 *     return Commit(new, delta)
 *
 * It is `edit` and not `commit` because there is nothing pending to commit.
 * A commit is what you do to changes a session has been accumulating, and
 * there is no session and no accumulation: you hand over text and get back
 * the document it describes, plus what changed. The receiver is SUPERSEDED —
 * released here and `*document` cleared on every path — so a caller cannot
 * hold both. */
bool markdown_core_document_edit(
    markdown_core_document **document,
    markdown_core_string markdown,
    markdown_core_commit *out,
    markdown_core_error **error
) {
    markdown_core_document *old;
    markdown_core_document *nw;
    markdown_core_delta *delta = NULL;

    clear_error(error);
    if (out) {
        out->document = NULL;
        out->delta = NULL;
    }
    if (!document || !*document) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document must not be null");
        return false;
    }
    if (!markdown.data && markdown.length != 0) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "markdown must not be null when length is nonzero"
        );
        return false;
    }
    old = *document;
    // The successor gets its OWN allocator. `old->mem` is the arena's own
    // face when the predecessor is pooled, and releasing the predecessor
    // releases that arena — so building into it and then releasing would free
    // the document this call returns.
    nw = document_build(
        &old->options,
        markdown,
        old,
        markdown_core_mem_default(),
        old->arena != NULL,
        out ? &delta : NULL,
        error
    );
    if (!nw) {
        return false;
    }
    markdown_core_document_release(old);
    *document = NULL;
    if (out) {
        out->document = nw;
        out->delta = delta;
    } else {
        markdown_core_document_release(nw);
    }
    return true;
}

/* TRANSITIONAL, test-only: the document's stored text. The engine's own
 * operation takes text in; this exists so an edit-shaped test can still ask
 * "what does it hold now" while those tests migrate to owning their own
 * string. It goes with markdown_core_document_splice. */
const uint8_t *markdown_core_document_text(const markdown_core_document *document, size_t *length) {
    size_t n = document ? markdown_core_source_length(document->source) : 0;
    size_t run = 0;
    if (length) {
        *length = n;
    }
    return n ? markdown_core_source_run_at(document->source, 0, &run) : (const uint8_t *)"";
}

uint64_t markdown_core_document_revision(const markdown_core_document *session) {
    return session ? session->revision : 0;
}

uint64_t markdown_core_document_lineage(const markdown_core_document *session) { return session ? session->lineage : 0; }

size_t markdown_core_document_length(const markdown_core_document *session) {
    return session ? markdown_core_source_length(session->source) : 0;
}

