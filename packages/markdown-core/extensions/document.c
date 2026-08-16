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

// A document is a handle onto its chain: the chain owns the TEXT (one buffer
// every handle shares, a document being a length watermark into it), the
// revision counter, the series salt, the poison flag, the base allocator, and
// ONE GENERATION — the head's tree, its diagnostics, and the parser that
// built it, kept at end of feed. A document names its text by watermark
// because appends only ever add at the end, so what a document describes is
// fixed the moment it is built even though the chain keeps growing.
//
// An append is one of two ticks, and which one is decided BEFORE anything is
// written (core/blocks.c, the eligibility predicate):
//
//   WARM  — the head's tree grows in place: the previous projection is
//           retracted, the chunk is fed, what the feed closed is settled, and
//           a new projection is published; ids hand over at the frontier and
//           the touched spine is restamped. Cost: the chunk, the units it
//           closed, and the open leaf. Prose shapes only, for now.
//   REBUILT — today's D6: a fresh parser over every byte so far, diffed
//           against the head for the (id, revision) handover. Exactly as
//           correct as the warm tick and O(document); every shape the warm
//           path does not take yet lands here, and the ledger counts it.
//
// Either way the equivalence suite pins the same thing: after every append
// the result equals a one-shot parse of the same text.

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

markdown_core_parser *markdown_core_document_new_parser(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    markdown_core_error **error
) {
    markdown_core_parser *parser = markdown_core_parser_new_with_mem(native_options_from(options), mem);
    if (!parser) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

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

/* The parser's diagnostics become the generation's, converted from the
 * core-side record to the facade type, and rebuilt whole at every close: a
 * warm generation's parser keeps raising them as the stream grows, and a
 * retract takes back the ones its close raised. A conversion that cannot
 * allocate leaves the generation with none: a missing underline is not a
 * wrong tree, and failing an otherwise good parse over one would be the
 * worse trade. */
static void generation_take_diagnostics(document_generation *generation, const markdown_core_parser *parser) {
    markdown_core_mem *mem = generation->mem;
    if (generation->diagnostics) {
        mem->free(mem, generation->diagnostics);
        generation->diagnostics = NULL;
    }
    generation->diagnostic_count = 0;
    if (parser->diagnostic_count > 0) {
        markdown_core_diagnostic *rows =
            (markdown_core_diagnostic *)mem->calloc(mem, parser->diagnostic_count, sizeof(*rows));
        if (rows) {
            size_t i;
            for (i = 0; i < parser->diagnostic_count; i++) {
                rows[i].code = (markdown_core_diagnostic_code)parser->diagnostics[i].code;
                rows[i].scope.start.line = parser->diagnostics[i].start_line;
                rows[i].scope.start.column = parser->diagnostics[i].start_column;
                rows[i].scope.end.line = parser->diagnostics[i].end_line;
                rows[i].scope.end.column = parser->diagnostics[i].end_column;
            }
            generation->diagnostics = rows;
            generation->diagnostic_count = parser->diagnostic_count;
        }
    }
}

/* Every allocation-loss route a parse has converges on `oom`: block and
 * inline structures set it directly, the definition maps carry their own
 * sticky flag, and a capture lost after the last line boundary — the
 * finalize-time harvest has no later line to fold it into — surfaces through
 * capture_lost. refine_blocks folds them before its verdict; a build that
 * closes some other way must fold them itself. */
static bool parser_failed(markdown_core_parser *parser) {
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (parser->capture_lost) {
        parser->oom = true;
    }
    /* A held partial line the feed could not grow is bytes the parse never
     * saw: a lost line, not a wrong tree — and a lost line is a failed
     * parse, whichever way the build closes. */
    if (parser->linebuf.oom) {
        parser->oom = true;
    }
    return parser->oom || parser->internal_error;
}

static void set_parse_error(const markdown_core_parser *parser, markdown_core_error **error) {
    bool allocation_failed = parser->oom;
    bool internal_error = parser->internal_error;
    markdown_core_ast_set_error(
        error,
        allocation_failed || !internal_error ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
        allocation_failed || !internal_error ? "could not parse the document text"
                                             : "parser refinement invariant failed"
    );
}

/* THE PREFIX FOLDS a record's spine blocks are restamped from. A spine
 * block's children before its saved youngest child are settled — the same
 * objects with the same hashes for the rest of the stream — so its stamp is
 * that fold continued over what grew, and a tick restamps a block in the
 * size of what grew rather than the size of the block: without this, the
 * root's restamp alone is a fold over every top-level block per tick, an
 * O(document) term the amortized bound cannot carry. `previous` may be
 * NULL; when a block was on the previous record too, its fold is carried
 * forward over the children that settled since (they were stamped by the
 * tick before this is called), and only a block new to the spine is folded
 * from its own fields. */
static void record_prefix_hashes(markdown_core_warm_undo *record, const markdown_core_warm_undo *previous) {
    size_t i;
    for (i = 0; i < record->spine_count; i++) {
        markdown_core_warm_open_block *entry = &record->spine[i];
        const markdown_core_node *node = entry->node;
        const markdown_core_warm_open_block *carried = NULL;
        uint64_t h;
        const markdown_core_node *child;
        size_t j;
        for (j = 0; previous && j < previous->spine_count; j++) {
            if (previous->spine[j].node == node) {
                carried = &previous->spine[j];
                break;
            }
        }
        if (!entry->last_child) {
            /* No child preceded the youngest: the fold is the block's own
             * fields alone. (An unrefined leaf at its save; the root of an
             * empty document. What hangs under it NOW — a leaf's tentative
             * inline children — is what the restamp folds, not the prefix.) */
            h = markdown_core_node_stamp_own(node);
        } else {
            if (carried && carried->last_child) {
                h = carried->prefix_hash;
                child = carried->last_child;
            } else {
                h = markdown_core_node_stamp_own(node);
                child = node->first_child;
            }
            for (; child && child != entry->last_child; child = child->next) {
                h = markdown_core_hash_mix(h, child->subtree_hash);
            }
        }
        entry->prefix_hash = h;
    }
}

/* HOW EVERY BUILD ENDS. The parser is at end of feed with the tree still
 * open. If its open state is one a publish can be retracted from — the
 * eligibility predicate, asked of the state alone since no chunk has
 * arrived — the closed part is settled once and for good and the spine is
 * PUBLISHED, so the record that comes back lets the next append reopen the
 * parser and grow this very tree. Otherwise the parser is closed for good:
 * the same two passes refine_blocks runs, without the detach that would
 * end its ownership, and no record — the next append rebuilds. Either way
 * the tree equals a one-shot parse of the same bytes, is stamped for the
 * diff, and the parser stays with the generation as the tree's owner. */
static bool generation_close(document_generation *generation, markdown_core_error **error) {
    markdown_core_parser *parser = generation->parser;

    if (markdown_core_parser_warm_eligible_at_eof(parser)) {
        /* Settle before close: exact here, because an eligible open state
         * harvests nothing at its close — the definition tables are already
         * what a one-shot parse would refine against. */
        markdown_core_parser_warm_settle(parser, NULL);
        generation->undo = markdown_core_parser_warm_publish(parser);
        if (!generation->undo) {
            markdown_core_ast_set_error(
                error,
                MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                "could not parse the document text"
            );
            return false;
        }
    } else {
        markdown_core_parser_finalize_blocks(parser);
        markdown_core_parser_warm_refine(parser);
    }
    if (parser_failed(parser)) {
        set_parse_error(parser, error);
        return false;
    }
    /* WHERE THE TREE IS FINGERPRINTED (see refine_blocks' note on why it is
     * one pass over the settled tree and nothing earlier): the append diff
     * pairs on these hashes, and a warm tick restamps only what it touches. */
    markdown_core_node_stamp_tree(parser->root);
    if (generation->undo) {
        record_prefix_hashes(generation->undo, NULL);
    }
    generation_take_diagnostics(generation, parser);
    return true;
}

/* PARSE. A pure function of (bytes, options): it fills this fresh generation
 * from the chain's stored bytes plus the ones this mutation brought, which
 * together are exactly the document's text. The parser's definition maps
 * serve its own inline phase and stay with it: the tree carries every
 * published answer. No predecessor, no ids — identity is the diff's to
 * assign. */
static bool document_parse_text(
    markdown_core_chain *chain,
    document_generation *generation,
    markdown_core_string arriving,
    markdown_core_error **error
) {
    markdown_core_parser *parser;

    parser = markdown_core_document_new_parser(&chain->options, generation->mem, error);
    if (!parser) {
        return false;
    }
    generation->parser = parser;

    // Fed in two pieces: the chain's stored bytes, then the ones this
    // mutation brought. They are separate only because the arriving bytes are
    // not stored yet — a mutation takes them only once it has succeeded — and
    // markdown_core_parser_feed is a streaming interface (core/blocks.c
    // S_parser_feed buffers a partial line in parser->linebuf), so where the
    // pieces are cut changes nothing about the parse.
    {
        size_t stored = markdown_core_source_length(chain->source);
        size_t pos = 0;
        while (pos < stored) {
            size_t run = 0;
            const uint8_t *bytes = markdown_core_source_run_at(chain->source, pos, &run);
            markdown_core_parser_feed(parser, (const char *)bytes, run);
            pos += run;
        }
        /* Unconditional: an empty chunk is a legal mutation, and a feed of
         * no bytes is defined to change nothing — including the pending-CR
         * seam a later chunk may still complete. */
        markdown_core_parser_feed(parser, (const char *)arriving.data, arriving.length);
    }
    return generation_close(generation, error);
}

/* THE WARM TICK: the head's tree grows in place. The record of the previous
 * publish is retracted, which reopens the spine and puts the held line back
 * while keeping what that close had appended — the frontier — detached on
 * the record; the chunk is fed; the units the feed closed are settled once
 * and for good; and a new projection is published. Then identity: for each
 * block that was open before the feed, deepest first, the run of children it
 * gained pairs against the retired frontier of the same block, the run is
 * stamped, and the block takes the tick's revision and a fresh stamp — so
 * revisions cover by touch up the spine, and a later rebuild's diff reads
 * hashes that are true at every level. Nothing else in the tree is visited.
 *
 * Only after eligibility said yes, and the caller has reserved the bytes: a
 * failure here leaves the chain poisoned, with a tree that is structurally
 * whole and semantically abandoned, which is what "only free remains" says. */
static bool document_tick_warm(
    markdown_core_chain *chain,
    markdown_core_string chunk,
    uint64_t revision,
    markdown_core_error **error
) {
    document_generation *generation = &chain->head;
    markdown_core_parser *parser = generation->parser;
    markdown_core_warm_undo *before = generation->undo;
    markdown_core_warm_undo *after = NULL;
    bool ok = false;
    size_t i;

    generation->undo = NULL;
    if (!markdown_core_parser_warm_retract(parser, before)) {
        parser->internal_error = true;
        goto done;
    }
    markdown_core_parser_feed(parser, (const char *)chunk.data, chunk.length);
    /* The predicate promised this state; the state is asked directly before
     * the close is allowed to touch it, because a close on a state the
     * record cannot put back is not merely wrong — a definitions-only
     * paragraph is FREED by its own finalize, and the record would name it.
     * A refusal here is the engine contradicting itself, and it poisons the
     * chain rather than publishing what it cannot reopen. */
    if (!markdown_core_parser_warm_eligible_at_eof(parser)) {
        parser->internal_error = true;
        goto done;
    }
    if (!markdown_core_parser_warm_settle(parser, before)) {
        /* A spine block replaced by its own refine: the predicate excludes
         * every shape that can, so this is the engine contradicting itself. */
        parser->internal_error = true;
        goto done;
    }
    after = markdown_core_parser_warm_publish(parser);
    if (!after || parser_failed(parser)) {
        goto done;
    }
    if (after->final) {
        /* The close retyped or replaced a spine block. The predicate keeps
         * every line that can out of a warm tick, so this too is the engine
         * contradicting itself — and the record's own spine may now name a
         * freed node, so nothing below may run. */
        parser->internal_error = true;
        goto done;
    }
    /* Identity and revision, deepest block first. The run a block gained is
     * stamped, then diffed against the block's retired frontier — the same
     * plan and machine a rebuild's diff runs — and the block itself takes
     * the tick's revision only if that diff or a deeper block says something
     * under it changed: an empty append moves no revision at all, and a byte
     * that grows the leaf moves the leaf's and every block above it. */
    {
        bool changed_below = false;
        i = before->spine_count;
        while (i-- > 0) {
            markdown_core_warm_open_block *entry = &before->spine[i];
            markdown_core_node *node = entry->node;
            markdown_core_node *run = entry->last_child ? entry->last_child->next : node->first_child;
            markdown_core_node *child;
            bool changed = false;
            for (child = run; child; child = child->next) {
                markdown_core_node_stamp_tree(child);
            }
            if (!markdown_core_diff_frontier(chain, generation->mem, entry->retired, run, revision, &changed)) {
                parser->oom = true;
                goto done;
            }
            changed_below = changed_below || changed;
            if (changed_below) {
                node->last_changed_rev = revision;
            }
            markdown_core_node_stamp_from(
                node,
                entry->prefix_hash,
                entry->last_child ? entry->last_child : node->first_child
            );
        }
    }
    record_prefix_hashes(after, before);
    generation_take_diagnostics(generation, parser);
    generation->undo = after;
    after = NULL;
    ok = true;

done:
    if (!ok) {
        if (after) {
            set_parse_error(parser, error);
        } else if (parser_failed(parser)) {
            set_parse_error(parser, error);
        } else {
            markdown_core_ast_set_error(
                error,
                MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                "could not parse the document text"
            );
        }
        markdown_core_parser_warm_undo_free(after);
    }
    markdown_core_parser_warm_undo_free(before);
    return ok;
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
    markdown_core_chain *chain,
    markdown_core_mem *mem,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_revision,
    markdown_core_error **error
) {
    if (!markdown_core_diff_trees(chain, mem, old_root, new_root, new_revision)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not match identities");
        return false;
    }
    return true;
}

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
/* Opens a generation to build into: its own arena when the chain pools, and
 * the allocator everything that build produces comes from. */
static bool generation_open(markdown_core_chain *chain, document_generation *generation, markdown_core_error **error) {
    memset(generation, 0, sizeof(*generation));
    if (chain->pooled) {
        generation->arena = markdown_core_arena_new(chain->mem);
        if (!generation->arena) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
            return false;
        }
        generation->mem = markdown_core_arena_mem(generation->arena);
    } else {
        generation->mem = chain->mem;
    }
    return true;
}

/* Releases a generation whole. Pooled, that is one arena release and the
 * parser, the tree, the record and the diagnostics go with it; unpooled, it
 * is the per-structure teardown the sanitizer builds need in order to see
 * each free: the record first (it may hold a retired frontier, detached
 * from the tree), then the parser, which frees the tree it owns. */
static void generation_release(document_generation *generation) {
    if (generation->arena) {
        markdown_core_arena_release(generation->arena);
    } else {
        markdown_core_parser_warm_undo_free(generation->undo);
        if (generation->parser) {
            markdown_core_parser_free(generation->parser);
        }
        if (generation->diagnostics && generation->mem) {
            generation->mem->free(generation->mem, generation->diagnostics);
        }
    }
    memset(generation, 0, sizeof(*generation));
}

static markdown_core_chain *chain_new(markdown_core_mem *mem, const markdown_core_parse_options *options, bool pooled) {
    markdown_core_chain *chain = (markdown_core_chain *)calloc(1, sizeof(*chain));
    if (chain) {
        uint64_t entropy = (uint64_t)(uintptr_t)chain;
        uint64_t host_entropy = 0;
        chain->refcount = 1;
        chain->next_revision = 1;
        chain->next_id = 1;
        chain->mem = mem;
        if (options) {
            chain->options = *options;
        } else {
            markdown_core_parse_options_init(&chain->options);
        }
#if MARKDOWN_CORE_ASAN
        // Slab-carved and freelist-reused blocks are invisible to
        // AddressSanitizer, so pooling would blind the ASan suites to
        // use-after-free and overflow inside a generation's memory. The
        // sanitizer build exercises the same allocation paths against the
        // base allocator instead.
        pooled = false;
#endif
        chain->pooled = pooled;
        chain->source = markdown_core_source_new(mem);
        if (!chain->source) {
            free(chain);
            return NULL;
        }
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
        generation_release(&chain->head);
        markdown_core_source_release(chain->source);
        free(chain);
    }
}

// --- public API -------------------------------------------------------------

/* BUILD ONE DOCUMENT. `new` starts a chain; `append` extends one, and takes
 * whichever tick the eligibility predicate allows — decided here, before a
 * byte is written anywhere:
 *
 *     warm:    tick(head, chunk)                     — the tree grows in place
 *     rebuilt: new = parse(all bytes); diff(head, new) — today's D6
 *
 * Both publish the same way: the successor is the head, the receiver is
 * superseded, the bytes are the chain's. */
static markdown_core_document *document_build(
    const markdown_core_parse_options *options,
    markdown_core_string markdown,
    const markdown_core_document *prev,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    markdown_core_chain *chain;
    markdown_core_document *doc;
    bool warm;

    clear_error(error);
    /* `options` and `pooled` describe a CHAIN, so they are read only when one
     * is being born; an append inherits both from the chain it extends. */
    if (prev) {
        chain = chain_retain(prev->chain);
    } else {
        chain = chain_new(mem, options, pooled);
        if (!chain) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
            return NULL;
        }
    }
    doc = (markdown_core_document *)calloc(1, sizeof(*doc));
    if (!doc) {
        chain_release(chain);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    doc->chain = chain;
    /* The revision is CLAIMED here (stamped into nodes below) but the counter
     * advances only on success, so a failed build burns no number and
     * adjacent published documents stay strictly +1. */
    doc->revision = prev ? chain->next_revision : 0;
    /* This document's text is everything the chain holds plus what arrived.
     * Room for the arriving bytes is taken BEFORE the parse so that storing
     * them afterwards cannot fail: the chain takes a mutation's bytes only
     * once that mutation has succeeded, which is what keeps the stored length
     * equal to the head's watermark at every instant a caller could look. */
    doc->length = markdown_core_source_length(chain->source) + markdown.length;
    if (!markdown_core_source_reserve(chain->source, markdown.length)) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            prev ? "could not append the chunk" : "could not allocate document"
        );
        markdown_core_document_free(doc);
        return NULL;
    }
    /* THE DECISION, a pure probe over the previous publish's record and the
     * arriving bytes: nothing has been retracted, fed or written yet, so a
     * refusal costs exactly this read. */
    warm = prev && chain->head.parser &&
           markdown_core_parser_warm_eligible(
               chain->head.parser,
               chain->head.undo,
               (const unsigned char *)markdown.data,
               markdown.length
           );
    if (warm) {
        if (!document_tick_warm(chain, markdown, doc->revision, error)) {
            markdown_core_document_free(doc);
            return NULL;
        }
        chain->warm_ticks++;
    } else {
        document_generation generation;
        if (!generation_open(chain, &generation, error)) {
            markdown_core_document_free(doc);
            return NULL;
        }
        if (!document_parse_text(chain, &generation, markdown, error) || !markdown_core_document_diff(
                                                                             chain,
                                                                             generation.mem,
                                                                             document_generation_root(&chain->head),
                                                                             document_generation_root(&generation),
                                                                             doc->revision,
                                                                             error
                                                                         )) {
            generation_release(&generation);
            markdown_core_document_free(doc);
            return NULL;
        }
        /* PUBLICATION of a rebuild, in one place: the new generation replaces
         * the head. What it replaces is released here rather than when its
         * handle is freed, because a superseded handle answers for no tree —
         * the chain keeps one, and this is the moment it changes hands. */
        generation_release(&chain->head);
        chain->head = generation;
        if (prev) {
            /* THE TICK LEDGER: this mutation rebuilt the document from
             * nothing, so it reparsed every byte the document describes —
             * which is what the bound is about, and why the bytes are
             * counted next to the ticks. */
            chain->rebuilt_ticks++;
            chain->rebuilt_bytes += (uint64_t)doc->length;
        }
    }
    markdown_core_source_commit(chain->source, markdown.data, markdown.length);
    if (prev) {
        /* SUPERSESSION, in one place: advancing the chain clock makes the
         * successor the head (its claimed revision is now the one behind the
         * clock) and stops the receiver matching, in the same increment. */
        chain->next_revision++;
    }
    return doc;
}

/* One owner, one teardown, one name. A handle owns nothing but its own cell:
 * the chain owns the text, the tree and the diagnostics, and the last handle
 * to let go takes all three with it. */
void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    chain_release(document->chain);
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

/* APPEND: the one mutation. Any byte split is legal — mid-UTF-8, mid-CRLF,
 * mid-line — because the successor's projection is defined as a fresh parse
 * of all bytes so far, and bytes are all the chunk adds. Whether the engine
 * gets there by growing the head's tree or by rebuilding it is decided per
 * chunk (document_build) and changes nothing a caller can see but the cost.
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

    if (!mutation_permitted(document, chunk, error)) {
        return NULL;
    }
    /* The chunk is all this call carries: the bytes before it are already the
     * chain's, and the successor describes them by watermark rather than by
     * copy. An empty chunk still mutates — the chain advances and the
     * receiver is superseded, and the successor's projection is
     * byte-identical to its predecessor's. The chain carries the options and
     * the pooling; an append brings only bytes. */
    successor = document_build(NULL, chunk, document, document->chain->mem, false, error);
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

size_t markdown_core_document_length(const markdown_core_document *document) { return document ? document->length : 0; }
