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
// An append is ONE TICK: the head's tree grows in place. The previous
// projection is retracted, the chunk is fed, what the feed closed is
// settled, and a new projection is published; ids hand over at the
// frontier by pairing, and revisions move by touch. Cost: the chunk, the
// units it closed, the units a definition in it flips, and the open leaf —
// never the document. There is no other kind of tick: every close is
// retractable, an extension that puts a payload behind a block's pointer
// must say how large it is or it is not attached, and a failed tick
// poisons the chain rather than falling back to anything.
//
// The equivalence suite pins the one thing that matters: after every
// append the result equals a one-shot parse of the same text.

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
            size_t count = 0;
            /* Live ones only, in source order: the parser raises in the
             * order it refines, and a unit refined again (a definition
             * flipped it) raised its own again at the end. An insertion
             * sort — stable, and the vector is already sorted but for those
             * few. */
            for (i = 0; i < parser->diagnostic_count; i++) {
                const struct markdown_core_parser_diagnostic *from = &parser->diagnostics[i];
                markdown_core_diagnostic row;
                size_t at = count;
                if (from->dead) {
                    continue;
                }
                row.code = (markdown_core_diagnostic_code)from->code;
                row.scope.start.line = from->start_line;
                row.scope.start.column = from->start_column;
                row.scope.end.line = from->end_line;
                row.scope.end.column = from->end_column;
                while (at > 0 && (rows[at - 1].scope.start.line > row.scope.start.line ||
                                  (rows[at - 1].scope.start.line == row.scope.start.line &&
                                   rows[at - 1].scope.start.column > row.scope.start.column))) {
                    rows[at] = rows[at - 1];
                    at--;
                }
                rows[at] = row;
                count++;
            }
            generation->diagnostics = rows;
            generation->diagnostic_count = count;
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

/* Whether a block's payload union holds plain values and no pointer, so
 * two of them compare byte for byte. A code or HTML block's literal, and an
 * extension's payload, are re-allocated by every close — the same bytes
 * behind a fresh pointer — and compare through the own-fold instead, which
 * reads what they point at. */
static bool payload_is_plain(const markdown_core_node *node) {
    if (node->extension) {
        return false;
    }
    switch (node->type) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
    case MARKDOWN_CORE_NODE_LIST:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return true;
    default:
        return false;
    }
}

/* HOW EVERY BUILD ENDS. The parser is at end of feed with the tree still
 * open. The closed part is settled once and for good and the spine is
 * PUBLISHED, so the record that comes back lets the next append reopen the
 * parser and grow this very tree; the tree equals a one-shot parse of the
 * same bytes, and the parser stays with the generation as the tree's owner.
 * A parser that failed publishes nothing. */
static bool generation_close(document_generation *generation, markdown_core_error **error) {
    markdown_core_parser *parser = generation->parser;

    if (!markdown_core_parser_warm_eligible_at_eof(parser)) {
        set_parse_error(parser, error);
        return false;
    }
    /* Settle, then close: the closed part is refined against the tables as
     * the feed left them, and what the close's own harvest changes about
     * that — a definitions paragraph open at end of feed — the publish's
     * flips re-refine, for this projection. */
    markdown_core_parser_warm_settle(parser, NULL);
    generation->undo = markdown_core_parser_warm_publish(parser);
    if (!generation->undo) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not parse the document text");
        return false;
    }
    if (parser_failed(parser)) {
        set_parse_error(parser, error);
        return false;
    }
    /* WHAT IS STAMPED: exactly the subtrees the next tick's frontier diff
     * will pair against — the run each spine block gained at this close,
     * and the children the close's flips gave their units — and nothing
     * else in the tree. A settled node's hash is read only when it stands
     * in such a run, or as a flipped unit's old child, and the tick stamps
     * both when it pairs them. */
    {
        markdown_core_warm_undo *record = generation->undo;
        size_t i;
        for (i = 0; i < record->spine_count; i++) {
            markdown_core_node *child;
            for (child = markdown_core_warm_run_first(&record->spine[i]); child;
                 child = markdown_core_warm_run_next(&record->spine[i], child)) {
                markdown_core_node_stamp_tree(child);
            }
        }
        for (i = 0; i < record->flip_count; i++) {
            markdown_core_node *child;
            for (child = record->flips[i].unit->first_child; child; child = child->next) {
                markdown_core_node_stamp_tree(child);
            }
        }
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
 * stamped for that pairing, and the block takes the tick's revision — so
 * revisions cover by touch up the spine. Then the units a definition
 * flipped, and nothing else in the tree is visited.
 *
 * Only with a record that can be reopened, and after the caller has
 * reserved the bytes: a failure here leaves the chain poisoned, with a tree
 * that is structurally whole and semantically abandoned, which is what
 * "only free remains" says. */
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
    bool revived = false;
    size_t i;

    generation->undo = NULL;
    /* A leaf paragraph the last close took (nothing but definitions), or
     * replaced (promoted to a formula block), is put back by the retract —
     * the same object, but to the identity ledger it left the tree when the
     * close published, and a retired id never returns: if this close
     * publishes it, it is a new node. */
    revived = before->spine_count > 0 && (before->spine[before->spine_count - 1].vanished ||
                                          before->spine[before->spine_count - 1].replaced != NULL);
    if (!markdown_core_parser_warm_retract(parser, before)) {
        /* Refused: a record already retracted (the engine contradicting
         * itself, since the caller read it), or an allocation lost while the
         * frontier took ownership of its bytes or a block got its bytes
         * back, which the parser's sticky bit already says. */
        if (!parser->oom) {
            parser->internal_error = true;
        }
        goto done;
    }
    markdown_core_parser_feed(parser, (const char *)chunk.data, chunk.length);
    markdown_core_parser_warm_settle(parser, before);
    after = markdown_core_parser_warm_publish(parser);
    if (!after || parser_failed(parser)) {
        goto done;
    }
    /* Identity and revision, deepest block first. The run a block gained is
     * stamped, then diffed against the block's retired frontier — hash
     * sweeps front and back, a positional middle by type, residue minted,
     * each pair classified by its fields and children — and the block itself
     * takes the tick's revision only if that diff or a deeper block says
     * something under it changed: an empty append moves no revision at all,
     * and a byte that grows the leaf moves the leaf's and every block above
     * it. */
    {
        bool changed_below = false;
        bool gone = false;
        /* THE LEAF THE FEED TOOK. A leaf paragraph that came to be nothing
         * but definitions left the tree at the feed's close — for good, and
         * the parser keeps it, unlinked, so this can know rather than read
         * it freed. Its frontier retires whole and its id with it; and its
         * parent's saved youngest child is no longer a child, so the
         * parent's run is everything after the sibling it followed, paired
         * against both of what the last close retired there, as one run. */
        if (before->spine_count > 1 &&
            markdown_core_parser_warm_vanished(parser, before->spine[before->spine_count - 1].node)) {
            markdown_core_warm_open_block *parent = &before->spine[before->spine_count - 2];
            gone = true;
            if (parent->retired_inserted) {
                markdown_core_node *tail = parent->retired_inserted;
                while (tail->next) {
                    tail = tail->next;
                }
                tail->next = parent->retired;
                if (parent->retired) {
                    parent->retired->prev = tail;
                }
                parent->retired = parent->retired_inserted;
                parent->retired_inserted = NULL;
            }
            parent->last_child = NULL;
        }
        i = before->spine_count;
        while (i-- > 0) {
            markdown_core_warm_open_block *entry = &before->spine[i];
            markdown_core_node *node = entry->node;
            markdown_core_node *appended;
            markdown_core_node *run;
            markdown_core_node *child;
            bool inserted;
            bool changed = false;
            if (gone && i == before->spine_count - 1) {
                /* Its leaving is a change only if it had been published: a
                 * leaf the last close had already taken was never in the
                 * tree a consumer saw. */
                changed_below = changed_below || !revived;
                continue;
            }
            appended =
                entry->last_child ? entry->last_child->next : (entry->prev ? entry->prev->next : node->first_child);
            /* A leaf the retract revived is a new object IF this close
             * published it (as itself, or as the block that replaced it
             * again); one that vanished again (the same definitions, an
             * empty chunk) left the tree as it found it, and moves nothing
             * — whatever id it carries, or does not. */
            if (revived && i == before->spine_count - 1) {
                if (!node->parent) {
                    continue;
                }
                markdown_core_diff_mint(chain, node, revision);
                changed_below = true;
            } else if (node->id == 0) {
                /* A spine block the settle's refine replaced is a new object
                 * standing where the old stood: minted, as any new node. */
                markdown_core_diff_mint(chain, node, revision);
                changed_below = true;
            }
            run = markdown_core_warm_run_first(entry);
            inserted = run != appended;
            /* Stamped here and nowhere else: these are the subtrees the
             * next tick's frontier diff will pair against. */
            for (child = run; child; child = markdown_core_warm_run_next(entry, child)) {
                markdown_core_node_stamp_tree(child);
            }
            /* The run pairs against the retired frontier in its two parts:
             * what the close INSERTED before the youngest child (the
             * definitions harvested out of it, a lead paragraph split off a
             * table) against what the last close inserted, and what was
             * appended after it against what the last close appended. */
            if (inserted) {
                if (!markdown_core_diff_frontier(
                        chain,
                        generation->mem,
                        entry->retired_inserted,
                        run,
                        entry->last_child && entry->last_child->parent == node ? entry->last_child : NULL,
                        revision,
                        &changed
                    )) {
                    parser->oom = true;
                    goto done;
                }
            }
            if (!markdown_core_diff_frontier(
                    chain,
                    generation->mem,
                    entry->retired,
                    appended,
                    NULL,
                    revision,
                    &changed
                )) {
                parser->oom = true;
                goto done;
            }
            /* The block's own projection: the feed may have retyped it (a
             * setext underline turns the open paragraph into a heading), and
             * the close writes into its payload (a list's tightness, a
             * table's count behind its pointer) — a value the last close
             * published too, and the same value keeps the revision. */
            {
                bool own_changed =
                    node->type != entry->published_type ||
                    markdown_core_node_stamp_own(node) != entry->published_own_hash ||
                    (payload_is_plain(node) && memcmp(&node->as, &entry->published_payload, sizeof(node->as)) != 0);
                changed = changed || own_changed;
                changed_below = changed_below || changed;
                if (changed_below) {
                    node->last_changed_rev = revision;
                }
            }
        }
    }
    /* FLIPS. A definition changed what settled units answered — for good,
     * by the feed (the parser lists them, each with its old children), or
     * for this projection only, by the close (the record lists them and
     * keeps their old children) — and the units the LAST close flipped were
     * left unrefined by the retract and refined again by the settle, against
     * tables that may or may not hold that definition still (the record
     * keeps what the flip had published). Each flipped unit keeps its
     * identity and its children as they stand pair against what was last
     * published under it, by the frontier's own diff, so a child the
     * definition did not touch keeps its id and revision; and if that diff
     * finds a change, the unit and every block above it take the tick's
     * revision — by touch, up the ancestors, whatever their width. A close's
     * flip of a unit refined THIS tick — one the feed appended, or one the
     * last close flipped — pairs against nothing from its record: those old
     * children were never published; the run they stand in, or the last
     * flip's published run, is what pairs. Both runs are stamped for the
     * pairing here: the old one may have been refined by a fresh build,
     * which stamps nothing it will not pair. */
    {
        size_t j;
        for (j = 0; j < parser->flipped_count + before->flip_count + after->flip_count; j++) {
            markdown_core_node *unit;
            markdown_core_node *old;
            markdown_core_node *child;
            bool changed = false;
            if (j < parser->flipped_count) {
                unit = parser->flipped[j].unit;
                old = parser->flipped[j].children;
            } else if (j < parser->flipped_count + before->flip_count) {
                unit = before->flips[j - parser->flipped_count].unit;
                old = before->flips[j - parser->flipped_count].published;
            } else {
                unit = after->flips[j - parser->flipped_count - before->flip_count].unit;
                old = after->flips[j - parser->flipped_count - before->flip_count].children;
            }
            /* Old children that were never published — a unit the retract
             * refined again and the feed then flipped for good, or a unit
             * refined this tick that the close flipped — pair nothing; the
             * published run they replaced pairs, on its own entry. */
            if (!old || old->id == 0) {
                continue;
            }
            for (child = old; child; child = child->next) {
                markdown_core_node_stamp_tree(child);
            }
            for (child = unit->first_child; child; child = child->next) {
                markdown_core_node_stamp_tree(child);
            }
            if (!markdown_core_diff_frontier(
                    chain,
                    generation->mem,
                    old,
                    unit->first_child,
                    NULL,
                    revision,
                    &changed
                )) {
                parser->oom = true;
                goto done;
            }
            if (changed) {
                for (child = unit; child; child = child->parent) {
                    child->last_changed_rev = revision;
                }
            }
        }
        markdown_core_parser_warm_flipped_free(parser);
        markdown_core_parser_warm_vanished_free(parser);
    }
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

/* BUILD ONE DOCUMENT. `new` starts a chain with a fresh parse, minted
 * whole; `append` extends one by growing the head's tree in place. Both
 * publish the same way: the successor is the head, the receiver is
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
    if (prev) {
        /* THE TICK: the head's tree grows in place. Every successful build
         * leaves a record that can be reopened, and a chain whose head has
         * none is one no mutation may reach (a failed tick poisons it), so
         * a head without a record is the engine contradicting itself. */
        if (!chain->head.parser || !chain->head.undo || chain->head.undo->retracted) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "the head has no record to reopen");
            markdown_core_document_free(doc);
            return NULL;
        }
        if (!document_tick_warm(chain, markdown, doc->revision, error)) {
            markdown_core_document_free(doc);
            return NULL;
        }
    } else {
        /* THE FIRST BUILD: a fresh parser over the text, minted whole. */
        document_generation generation;
        if (!generation_open(chain, &generation, error)) {
            markdown_core_document_free(doc);
            return NULL;
        }
        if (!document_parse_text(chain, &generation, markdown, error)) {
            generation_release(&generation);
            markdown_core_document_free(doc);
            return NULL;
        }
        markdown_core_diff_mint(chain, document_generation_root(&generation), doc->revision);
        generation_release(&chain->head);
        chain->head = generation;
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
 * of all bytes so far, and bytes are all the chunk adds; the engine gets
 * there by growing the head's tree (document_build), which changes nothing
 * a caller can see but the cost.
 *
 * A failed append poisons the chain (D5): the engine cannot say which side
 * of the failure the chunk landed on, the caller still holds every byte it
 * ever sent, and recovery is a new document — so the chain reports itself
 * done rather than pretending to a state it cannot prove. */
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
