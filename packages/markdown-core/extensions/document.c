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

// A document owns its text, its committed tree, and its diagnostics, and
// shares one object with the other handles on its chain: the chain record
// (refcount, revision counter, series salt, poison flag, base allocator).
// Every append is one full parse of all bytes so far plus a diff against the
// previous document (document_build below); the equivalence suite pins that
// the result always equals a one-shot parse of the same text.

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

// --- parsing ----------------------------------------------------------------

static int native_options_from(const markdown_core_parse_options *options) {
    /* UTF-8 is assumed and never validated; nothing is switched on here to
     * check it. */
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

/* PARSE. A pure function of (bytes, options): it fills this freshly built
 * document's tree from its own stored text and reads nothing else. The
 * parser's definition maps serve the parse's own inline phase and die with
 * the parser: the tree carries every published answer. No predecessor, no
 * ids — identity is the diff's to assign. */
static bool document_parse_text(markdown_core_document *document, markdown_core_error **error) {
    markdown_core_parser *parser;
    markdown_core_node *root;

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
    root = markdown_core_parser_refine_blocks(parser);
    if (!root) {
        bool allocation_failed = parser->oom;
        bool internal_error = parser->internal_error;
        markdown_core_parser_free(parser); // frees the definition maps with it
        markdown_core_ast_set_error(
            error,
            allocation_failed || !internal_error ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
            allocation_failed || !internal_error ? "could not parse the document text"
                                                 : "parser refinement invariant failed"
        );
        return false;
    }
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
    document->root = root;
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
    if (!markdown_core_diff_trees(nw, old ? old->root : NULL, nw->root, nw->revision)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not match identities");
        return false;
    }
    return true;
}

static markdown_core_document *markdown_core_document_alloc(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
);

/* The chain owner's one atomic: handles on a chain may be freed from
 * different threads, so the refcount is advanced with a compiler builtin
 * rather than <stdatomic.h> — this engine is built as C99 on four
 * toolchains, and MSVC's C11 atomics are not there yet. Everything else on
 * the chain is owned by the current mutation, which the contract serializes
 * externally. */
#if defined(_MSC_VER)
#include <intrin.h>
static uint32_t chain_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)slot, (long)amount);
}
#elif defined(__GNUC__) || defined(__clang__)
static uint32_t chain_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    return __atomic_fetch_add(slot, amount, __ATOMIC_SEQ_CST);
}
#else
/* No builtins: single-threaded targets only, where the plain read-modify-write
 * is already indivisible with respect to anything that could observe it. */
static uint32_t chain_fetch_add32(volatile uint32_t *slot, int32_t amount) {
    uint32_t previous = *slot;
    *slot = (uint32_t)((int32_t)previous + amount);
    return previous;
}
#endif

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

/* Born once per chain, at document_new/open; append retains the receiver's.
 * The series salt is minted HERE, so an append pays no host-entropy read:
 * every document on the chain shares this one value. The address/time/clock
 * mix alone is deterministic for the first chain of lockstep-started isolated
 * runtimes (one WASM instance per worker reproduces the same allocator state
 * and coarse clocks), so the host CSPRNG carries the cross-runtime uniqueness
 * contract; the local mix stays folded in as a best-effort fallback when the
 * host read fails. */
static markdown_core_chain *chain_new(markdown_core_mem *mem) {
    markdown_core_chain *chain = (markdown_core_chain *)calloc(1, sizeof(*chain));
    if (chain) {
        uint64_t entropy = (uint64_t)(uintptr_t)chain;
        uint64_t host_entropy = 0;
        chain->refcount = 1;
        chain->next_revision = 1;
        chain->mem = mem;
        entropy ^= markdown_core_mix64((uint64_t)time(NULL));
        entropy ^= markdown_core_mix64((uint64_t)clock()) << 1;
        if (document_host_entropy(&host_entropy)) {
            entropy ^= host_entropy;
        }
        chain->series = markdown_core_mix64(entropy);
    }
    return chain;
}

static markdown_core_chain *chain_retain(markdown_core_chain *chain) {
    if (chain) {
        chain_fetch_add32(&chain->refcount, 1);
    }
    return chain;
}

static void chain_release(markdown_core_chain *chain) {
    if (chain && chain_fetch_add32(&chain->refcount, -1) == 1) {
        free(chain);
    }
}

// --- public API -------------------------------------------------------------

/* BUILD ONE DOCUMENT FROM TEXT, and diff it against `prev` if there is one.
 *
 *     new = Document(markdown, options)
 *     diff(prev, new)
 *
 * That is the whole of both constructors — new starts a chain, append
 * extends one. There is no incremental path, no reuse of the previous tree:
 * the previous document is INPUT to the diff and nothing else, and it is
 * untouched by this call. */
static markdown_core_document *document_build(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
    const markdown_core_document *prev,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    markdown_core_document *doc;

    doc = markdown_core_document_alloc(options, markdown, mem, pooled, error);
    if (!doc) {
        return NULL;
    }
    if (prev) {
        doc->chain = chain_retain(prev->chain);
        doc->next_id = prev->next_id;
        /* The revision is CLAIMED here (the diff below stamps it into nodes)
         * but the counter advances only on success, so a failed build burns
         * no number and adjacent published documents stay strictly +1. */
        doc->revision = prev->chain->next_revision;
    } else {
        doc->chain = chain_new(mem);
        if (!doc->chain) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
            markdown_core_document_free(doc);
            return NULL;
        }
        doc->revision = 0;
    }
    if (!document_parse_text(doc, error) || !markdown_core_document_diff(prev, doc, error)) {
        markdown_core_document_free(doc);
        return NULL;
    }
    if (prev) {
        /* SUPERSESSION, in one place: advancing the chain clock makes the
         * successor the head (its claimed revision is now the one behind the
         * clock) and stops the receiver matching, in the same increment. */
        doc->chain->next_revision++;
    }
    return doc;
}

/* Allocates a document holding `markdown`'s bytes. No parse; document_build
 * does that once, after wiring the chain. */
static markdown_core_document *markdown_core_document_alloc(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
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
    // The store holds whatever bytes it is handed. UTF-8 is assumed and
    // never validated, and a streamed append completes a multi-byte
    // character whose first bytes arrived earlier — deciding that at the
    // substrate was always the wrong layer, and there is no longer a
    // profile that could.
    document->source = markdown_core_source_new(mem, markdown.data, markdown.length);
    if (!document->source) {
        // Unwound by hand: only the arena and the bare struct exist yet.
        if (document->arena) {
            markdown_core_arena_release(document->arena);
        }
        free(document);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    document->next_id = 1;
    document->revision = 0;
    return document;
}

/* One owner, one teardown, one name. */
void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    document->mem->free(document->mem, document->diagnostics);
    /* The chain is a system allocation shared across the arena boundary, so
     * BOTH teardown paths release it. (Its predecessor, the series clock,
     * was released only on the unpooled path — every pooled document leaked
     * its cell, and the ASan suites could not see it because they force
     * pooling off.) */
    chain_release(document->chain);
    if (document->arena) {
        // Everything else the document owns came from the arena (errors are
        // caller-owned system allocations); one release replaces the
        // per-structure teardown below.
        markdown_core_arena_release(document->arena);
        free(document);
        return;
    }
    if (document->root) {
        markdown_core_node_free(document->root);
    }
    markdown_core_source_release(document->source);
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

/* THE MUTATION GUARDS. A mutation is legal only
 * on the chain's live head — a superseded handle fails deterministically,
 * which is what keeps history linear and lets a consumer destroy and
 * rebuild derived state in place — and never on a poisoned chain. Argument
 * validation fails the CALL, not the chain: a rejected argument touched
 * nothing, is repeatable, and poisoning over it would punish the caller for
 * a typo. */
static bool mutation_permitted(
    const markdown_core_document *document,
    markdown_core_string markdown,
    markdown_core_error **error
) {
    clear_error(error);
    if (!document) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document must not be null");
        return false;
    }
    if (document->chain->poisoned) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "the chain is done: a failed append ended it, and only free remains"
        );
        return false;
    }
    if (document->revision + 1 != document->chain->next_revision) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "the document has been superseded: mutate the successor"
        );
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
    return true;
}

/* APPEND: the one mutation. Any byte
 * split is legal — mid-UTF-8, mid-CRLF, mid-line — because the successor's
 * projection is defined as a fresh parse of all bytes so far, and bytes are
 * all the chunk adds. The implementation is exactly that definition:
 * concatenate and rebuild whole, then diff against the receiver for the
 * (id, revision) handover.
 *
 * A failed append poisons the chain (D5): the engine cannot say which side
 * of the failure the chunk landed on, the caller still holds every byte it
 * ever sent, and recovery is a rebuild — so the chain reports itself done
 * rather than pretending to a state it cannot prove. */
markdown_core_document *markdown_core_document_append(
    markdown_core_document *document,
    markdown_core_string chunk,
    markdown_core_error **error
) {
    markdown_core_document *successor;
    uint8_t *joined;
    size_t length;
    size_t total;
    size_t pos = 0;
    markdown_core_string whole;

    if (!mutation_permitted(document, chunk, error)) {
        return NULL;
    }
    /* One flat buffer of bytes-so-far plus the chunk. An empty chunk still
     * mutates — the chain advances and the receiver is superseded, and the
     * successor's projection is byte-identical to its predecessor's. */
    length = markdown_core_source_length(document->source);
    total = length + chunk.length;
    joined = (uint8_t *)malloc(total ? total : 1);
    if (!joined) {
        document->chain->poisoned = true;
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not append the chunk");
        return NULL;
    }
    while (pos < length) {
        size_t run = 0;
        const uint8_t *bytes = markdown_core_source_run_at(document->source, pos, &run);
        memcpy(joined + pos, bytes, run);
        pos += run;
    }
    if (chunk.length) {
        memcpy(joined + length, chunk.data, chunk.length);
    }
    whole.data = joined;
    whole.length = total;

    successor =
        document_build(&document->options, whole, document, document->chain->mem, document->arena != NULL, error);
    free(joined);
    if (!successor) {
        document->chain->poisoned = true;
        return NULL;
    }
    return successor;
}

uint64_t markdown_core_document_revision(const markdown_core_document *document) {
    return document ? document->revision : 0;
}

uint64_t markdown_core_document_series(const markdown_core_document *document) {
    return document ? document->chain->series : 0;
}

size_t markdown_core_document_length(const markdown_core_document *document) {
    return document ? markdown_core_source_length(document->source) : 0;
}
