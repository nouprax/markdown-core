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

// A document is a purely local object: it owns its text, its committed tree,
// and its definition tables, and shares nothing with any other document but
// its series' revision counter. Every edit is one full parse of the new
// text plus a diff against the previous document (document_build below); the
// equivalence suite pins that the result always equals a one-shot parse of
// the same text.

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

markdown_core_parser *markdown_core_document_new_parser(markdown_core_document *document, markdown_core_error **error) {
    markdown_core_parser *parser =
        markdown_core_parser_new_with_mem(native_options_from(&document->options), document->mem);
    if (!parser) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

    const markdown_core_parse_options *options = &document->options;
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
// piece of document state at once. The staging never touches the committed
// state, so any failure leaves the document valid at its previous revision.
/* PARSE. A pure function of (bytes, options): it fills this document's tree,
 * definition maps and footnote index from its own stored text and reads
 * nothing else. No predecessor, no ids — identity is the diff's to assign. */
static bool document_parse_text(markdown_core_document *document, markdown_core_error **error) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    markdown_core_definition_table staged[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    int total_lines;
    int last_line_length;
    size_t s;

    parser = markdown_core_document_new_parser(document, error);
    if (!parser) {
        return false;
    }

    // Fed run by run. markdown_core_parser_feed is a streaming interface
    // (core/blocks.c S_parser_feed buffers a partial line in parser->linebuf),
    // so the chunking is free to follow whatever the store hands back — which,
    // now that the store is one flat buffer, is the whole document at once.
    size_t length = markdown_core_source_length(document->source);
    {
        size_t pos = 0;
        while (pos < length) {
            size_t run = 0;
            const uint8_t *bytes = markdown_core_source_run_at(document->source, pos, &run);
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
            allocation_failed || !internal_error ? "could not parse the document text"
                                                 : "parser refinement invariant failed"
        );
        return false;
    }
    memset(staged, 0, sizeof(staged));
    staged[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map = parser->refmap;
    staged[MARKDOWN_CORE_DEFINITIONS_FOOTNOTES].map = parser->footnote_defs;
    parser->refmap = NULL;
    parser->footnote_defs = NULL;
    /* The parser's diagnostics become this document's, converted from the
     * core-side record to the facade type. A conversion that cannot allocate
     * leaves the document with none: a missing underline is not a wrong
     * tree, and failing an otherwise good parse over one would be the worse
     * trade. */
    document->mem->free(document->mem, document->diagnostics);
    document->diagnostics = NULL;
    document->diagnostic_count = 0;
    if (parser->diagnostic_count > 0) {
        markdown_core_diagnostic *rows =
            (markdown_core_diagnostic *)document->mem->calloc(document->mem, parser->diagnostic_count, sizeof(*rows));
        if (rows) {
            size_t i;
            for (i = 0; i < parser->diagnostic_count; i++) {
                rows[i].code = (markdown_core_diagnostic_code)parser->diagnostics[i].code;
                rows[i].scope.start.line = parser->diagnostics[i].start_line;
                rows[i].scope.start.column = parser->diagnostics[i].start_column;
                rows[i].scope.end.line = parser->diagnostics[i].end_line;
                rows[i].scope.end.column = parser->diagnostics[i].end_column;
            }
            document->diagnostics = rows;
            document->diagnostic_count = parser->diagnostic_count;
        }
    }
    markdown_core_parser_free(parser);
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        // The sink's context is this call's stack frame; the maps outlive it.
        // Both are present: a parse that lost one is poisoned and returns no
        // root, so this line is only reached with the tree in hand.
        staged[s].map->lookup_sink = NULL;
        staged[s].map->lookup_context = NULL;
        staged[s].map->lookup_unit = NULL;
    }
    if (false) {
        release_definition_tables(document->mem, staged);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not record the document's reference lookups"
        );
        return false;
    }

    memcpy(document->definitions, staged, sizeof(staged));
    document->root = root;
    document->total_lines = total_lines;
    document->last_line_length = last_line_length;
    return true;
}

/* DIFF. `new` has a tree and no identities; `old` may be NULL. This assigns
 * `new`'s identities from `old` — the one decision both requirements rest on
 * — and stamps each node's revision. It reads no text and reparses nothing.
 *
 * A diff of the same two trees is the same diff. That is the invariant the
 * old `adopt` name hid, and keeping parse and diff apart is what makes it
 * statable: a tree is a pure function of (bytes, options), and the identity
 * assignment is a pure function of two trees. */
bool markdown_core_document_diff(
    const markdown_core_document *old,
    markdown_core_document *nw,
    markdown_core_error **error
) {
    size_t s;

    if (!markdown_core_diff_trees(nw, old ? old->root : NULL, nw->root, nw->revision)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not match identities");
        return false;
    }
    // Ids exist now, so definitions recorded against anchor node pointers can
    // take their ids.
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_document_resolve_definition_owners(nw->definitions[s].map);
    }

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
static bool document_set_text(markdown_core_document *doc, markdown_core_string markdown, markdown_core_error **error) {
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

/* THE SERIES CLOCK. One cell per series, shared by every document in it and
 * released with the last of them.
 *
 * It exists because two successors of ONE predecessor must not share a
 * revision: a node that each of them changed differently would otherwise
 * carry one (id, revision) with two contents, which the contract says cannot
 * happen. Copying `prev->revision + 1` into both gives them the same number;
 * a moment from the host clock gives them different numbers ALMOST always,
 * and almost is not a contract -- the concurrency runner caught two threads
 * inside one microsecond on its first run under a sanitizer. So the number
 * comes from a counter they share and advance atomically.
 *
 * The atomics are compiler builtins, not <stdatomic.h>: this engine is built
 * as C99 on four toolchains, and MSVC's C11 atomics are not there yet. */
#if defined(_MSC_VER)
#include <intrin.h>
static uint64_t series_fetch_add(volatile uint64_t *slot, uint64_t amount) {
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long *)slot, (long long)amount);
}
static uint32_t series_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)slot, (long)amount);
}
#elif defined(__GNUC__) || defined(__clang__)
static uint64_t series_fetch_add(volatile uint64_t *slot, uint64_t amount) {
    return __atomic_fetch_add(slot, amount, __ATOMIC_SEQ_CST);
}
static uint32_t series_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    return __atomic_fetch_add(slot, amount, __ATOMIC_SEQ_CST);
}
#else
/* No builtins: single-threaded targets only, where the plain read-modify-write
 * is already indivisible with respect to anything that could observe it. */
static uint64_t series_fetch_add(volatile uint64_t *slot, uint64_t amount) {
    uint64_t previous = *slot;
    *slot = previous + amount;
    return previous;
}
static uint32_t series_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    uint32_t previous = *slot;
    *slot = (uint32_t)((int32_t)previous + amount);
    return previous;
}
#endif

static markdown_core_series_clock *series_clock_new(void) {
    markdown_core_series_clock *clock = (markdown_core_series_clock *)calloc(1, sizeof(*clock));
    if (clock) {
        clock->next_revision = 1;
        clock->refcount = 1;
    }
    return clock;
}

static markdown_core_series_clock *series_clock_retain(markdown_core_series_clock *clock) {
    if (clock) {
        series_fetch_add32(&clock->refcount, 1);
    }
    return clock;
}

static void series_clock_release(markdown_core_series_clock *clock) {
    if (clock && series_fetch_add32(&clock->refcount, -1) == 1) {
        free(clock);
    }
}

// One 64-bit read from the host CSPRNG. Documents stay free of any library-
// owned RNG state: every source below is the platform's own, shared-nothing
// entropy service.
static bool document_host_entropy(uint64_t *value) {
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

/* BUILD ONE DOCUMENT FROM TEXT, and diff it against `prev` if there is one.
 *
 *     new = Document(markdown, options)
 *     diff(prev, new)
 *
 * That is the whole of an edit. There is no incremental path, no pending
 * edit, no reuse of the previous tree: the previous document is INPUT to the
 * diff and nothing else, and it is untouched by this call. */
static markdown_core_document *document_build(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
    const markdown_core_document *prev,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    markdown_core_document *doc;

    doc = markdown_core_document_alloc(options, mem, pooled, error);
    if (!doc) {
        return NULL;
    }
    if (prev) {
        doc->series = prev->series;
        doc->next_id = prev->next_id;
        // The series' own counter, advanced once per successor: no two
        // successors of one predecessor can come away with the same number,
        // however they are scheduled.
        series_clock_release(doc->clock);
        doc->clock = series_clock_retain(prev->clock);
        doc->revision = doc->clock ? series_fetch_add(&doc->clock->next_revision, 1) : prev->revision + 1;
    }
    if (markdown.length && !document_set_text(doc, markdown, error)) {
        markdown_core_document_free(doc);
        return NULL;
    }
    if (!document_parse_text(doc, error) || !markdown_core_document_diff(prev, doc, error)) {
        markdown_core_document_free(doc);
        return NULL;
    }
    return doc;
}

/* Allocates a document and its empty source. No parse; document_build does
 * that once, with the text in hand. */
static markdown_core_document *markdown_core_document_alloc(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    clear_error(error);

    markdown_core_document *document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }

    if (options) {
        document->options = *options;
    } else {
        markdown_core_parse_options_init(&document->options);
    }
#if MARKDOWN_CORE_ASAN
    // Slab-carved and freelist-reused blocks are invisible to
    // AddressSanitizer, so pooling would blind the ASan suites to
    // use-after-free and overflow inside document memory. The sanitizer
    // build exercises the exact same allocation paths against the base
    // allocator instead.
    pooled = false;
#endif
    if (pooled) {
        document->arena = markdown_core_arena_new(mem);
        if (!document->arena) {
            free(document);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
            return NULL;
        }
        mem = markdown_core_arena_mem(document->arena);
    }
    document->mem = mem;
    {
        // The store holds whatever bytes it is handed. UTF-8 is assumed and
        // never validated (7.1), and a streamed append completes a multi-byte
        // character whose first bytes arrived earlier — deciding that at the
        // substrate was always the wrong layer, and there is no longer a
        // profile that could.
        markdown_core_source_stats scratch;
        markdown_core_source_status status;
        memset(&scratch, 0, sizeof(scratch));
        document->source = markdown_core_source_new(mem, NULL, 0, &scratch, &status);
        if (!document->source) {
            // Unwound here rather than through markdown_core_document_release:
            // that path releases document->source unconditionally, and it is
            // the thing that just failed to exist.
            if (document->arena) {
                markdown_core_arena_release(document->arena);
            }
            free(document);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
            return NULL;
        }
    }
    document->next_id = 1;
    document->revision = 0;
    document->clock = series_clock_new();

    // The address/time/clock mix alone is deterministic for the first
    // document of lockstep-started isolated runtimes (one WASM instance per
    // worker reproduces the same allocator state and coarse clocks), so the
    // host CSPRNG carries the cross-runtime uniqueness contract. The local
    // mix stays folded in as a best-effort fallback when the host read
    // fails.
    uint64_t entropy = (uint64_t)(uintptr_t)document;
    uint64_t host_entropy = 0;
    entropy ^= markdown_core_mix64((uint64_t)time(NULL));
    entropy ^= markdown_core_mix64((uint64_t)clock()) << 1;
    if (document_host_entropy(&host_entropy)) {
        entropy ^= host_entropy;
    }
    document->series = markdown_core_mix64(entropy);
    return document;
}

/* One owner, one teardown, one name. */
void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    document->mem->free(document->mem, document->diagnostics);
    if (document->arena) {
        // Everything the document owns came from the arena (errors are
        // caller-owned system allocations); one release replaces the
        // per-structure teardown below.
        markdown_core_arena_release(document->arena);
        free(document);
        return;
    }
    if (document->root) {
        markdown_core_node_free(document->root);
    }
    series_clock_release(document->clock);
    markdown_core_source_release(document->source);
    release_definition_tables(document->mem, document->definitions);
    free(document);
}

markdown_core_document *markdown_core_document_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    markdown_core_string empty = {NULL, 0};
    return document_build(options, empty, NULL, mem, pooled, error);
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
    return document_build(options, markdown, NULL, markdown_core_mem_default(), true, error);
}

/* EDIT: hand the document new text.
 *
 *     let new = Document(markdown, document.options)
 *     diff(document, new)
 *     return new
 *
 * It is `edit` and not `commit` because there is nothing pending to commit.
 * A commit is what you do to changes a document has been accumulating, and
 * there is no document and no accumulation: you hand over text and get back
 * the document it describes. The receiver is READ, not consumed: it is the
 * diff's input, it keeps every byte it owns, and it remains the caller's to
 * free. Editing it twice is therefore legal and gives two lines of descent,
 * told apart by their revisions. */
markdown_core_document *markdown_core_document_edit(
    const markdown_core_document *document,
    markdown_core_string markdown,
    markdown_core_error **error
) {
    clear_error(error);
    if (!document) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document must not be null");
        return NULL;
    }
    if (!markdown.data && markdown.length != 0) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "markdown must not be null when length is nonzero"
        );
        return NULL;
    }
    // The receiver is READ, never taken: it is the diff's input and nothing
    // else, it keeps every byte it owns, and the caller frees it when the
    // caller is done with it. That is what lets a document be shared across
    // threads without a lock and edited more than once — two successors of
    // one predecessor are two lines of descent, told apart by their revisions.
    return document_build(
        &document->options,
        markdown,
        document,
        markdown_core_mem_default(),
        document->arena != NULL,
        error
    );
}

uint64_t markdown_core_document_revision(const markdown_core_document *document) {
    return document ? document->revision : 0;
}

uint64_t markdown_core_document_series(const markdown_core_document *document) {
    return document ? document->series : 0;
}

size_t markdown_core_document_length(const markdown_core_document *document) {
    return document ? markdown_core_source_length(document->source) : 0;
}
