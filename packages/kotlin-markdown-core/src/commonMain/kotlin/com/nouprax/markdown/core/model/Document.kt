@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass
@file:OptIn(kotlin.concurrent.atomics.ExperimentalAtomicApi::class)

package com.nouprax.markdown.core

import kotlin.concurrent.atomics.AtomicLong
import kotlin.jvm.JvmOverloads

/** Everything one decoded payload settles, so that a public constructor can
 * delegate to the private one in a single expression. */
private class Built(
    val handle: CDocumentHandle,
    val id: MarkupID,
    val revision: ULong,
    val scope: Scope,
    val options: ParseOptions,
    val content: kotlin.collections.List<Markup>,
    val diagnostics: kotlin.collections.List<Diagnostic>,
    val index: Map<ULong, Markup>,
)

private fun open(
    markdown: String,
    options: ParseOptions,
): Built =
    decodeWire(cOpen(markdown.encodeToByteArray(), options)) {
        handle,
        id,
        revision,
        scope,
        content,
        index,
        diagnostics,
        ->
        Built(handle, id, revision, scope, options, content, diagnostics, index)
    }

/**
 * One parsed Markdown document: the root of the canonical value tree, the
 * owner of the native parse it came from, and the only entry point.
 *
 * ```kotlin
 * Document("# Title").use { document ->
 *     document.append("\n\nBody").close()
 * }
 * ```
 *
 * There is no session type. A document is the live head of a CHAIN: [edit]
 * hands it whole new text, [append] hands it trailing bytes, and either
 * mutation returns the next document and SUPERSEDES this one — the successor
 * is the new head, and a superseded document refuses further mutation with
 * the same deterministic error whichever way it is asked. Its decoded values
 * — content, scopes, diagnostics, dump, [node] — stay valid forever, and
 * [close] stays both legal and owed. Options are fixed for the chain's whole
 * life — changing what the parser means is a new [Document], not a mutation.
 *
 * What changed is asked of the new tree: an unchanged node keeps its
 * [Markup.id] and [Markup.revision], and a changed one carries the new
 * document's revision.
 *
 * The node values are ordinary immutable values with no reference back here:
 * hold them, put them in a view model, hand them to another thread. What this
 * class owns is the native parse, which a mutation needs in order to keep
 * identities stable across revisions — and which [close] releases. A document
 * that is never closed is released once it becomes unreachable, but that is a
 * backstop, not the contract.
 */
public class Document private constructor(
    private val built: Built,
) : Markup,
    AutoCloseable {
    /** Parses [markdown].
     *
     * Throws [ParseException] when the engine rejects the call or cannot
     * allocate. */
    @JvmOverloads
    public constructor(markdown: String, options: ParseOptions = ParseOptions()) : this(open(markdown, options))

    /** The native parse, or zero once something has taken it.
     *
     * Both paths that END the parse — [close] and the platform's reclaim —
     * go through this one exchange, and only the first wins. A mutation is
     * not one of them: superseding leaves the parse here for [close], which
     * on a superseded handle is an O(1) release of a thin shell. */
    private val handle = AtomicLong(built.handle)

    /** Nonzero once a mutation has succeeded and this document stopped being
     * its chain's head. The flag lives on the wrapper because the check must
     * not cost a native crossing: a superseded document answers reads from
     * its decoded values and refuses mutation deterministically, with the
     * engine's own wording. */
    private val superseded = AtomicLong(0L)

    // Held so the platform's reclaim registration outlives this constructor;
    // see attachNativeCleanup. Never read.
    @Suppress("unused")
    private val cleanup: Any? =
        run {
            // The action captures the handle slot and NOTHING else: a cleanup
            // that could reach this document would keep it reachable forever.
            val slot = handle
            attachNativeCleanup(this) {
                val owned = slot.exchange(0L)
                if (owned != 0L) {
                    owned.release()
                }
            }
        }

    override val id: MarkupID get() = built.id

    override val revision: ULong get() = built.revision

    /** The document's extent is the whole text. */
    override val scope: Scope get() = built.scope

    /** The options this document and its whole series were parsed under. */
    public val options: ParseOptions get() = built.options

    /** The document's top-level blocks in source order. */
    public val content: kotlin.collections.List<Markup> get() = built.content

    /** Everything an editor should underline, in source order.
     *
     * Empty for almost every document; see [DiagnosticCode]. */
    public val diagnostics: kotlin.collections.List<Diagnostic> get() = built.diagnostics

    /**
     * Per-series random salt.
     *
     * Nodes from different parses never compare equal even when their raw
     * ids collide numerically.
     */
    public val series: ULong get() = id.series

    /** [series] as a bit-preserving signed value: the Java view of the
     * unsigned accessor, whose mangled name Java sources cannot write. */
    public fun seriesBits(): Long = series.toLong()

    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)

    /** The handle a mutation may target: this document must be neither
     * closed nor superseded. The superseded refusal repeats the engine's own
     * wording, because it is the engine's rule enforced one crossing early. */
    private fun mutableHandle(): CDocumentHandle {
        val owned = handle.load()
        check(owned != 0L) { "the document is closed" }
        check(superseded.load() == 0L) { "the document has been superseded: mutate the successor" }
        return owned
    }

    /**
     * Hands this document new text and returns the document that text
     * describes, which SUPERSEDES this one.
     *
     * The successor is its chain's new head; this document keeps its decoded
     * values and its [close], and refuses further mutation deterministically.
     * A failed edit supersedes nothing: this document stays the head and may
     * be mutated again.
     *
     * An edit's answer is decoded whole, because an edit may shift a node's
     * position without changing its content — and position is not content,
     * so the revision does not move with it. Identity still survives: an
     * unchanged node keeps its exact (id, revision) pair, which is what
     * keeps a reactive consumer's per-id state alive across a correction.
     *
     * What changed is asked of the returned tree, not of a delta: a node's
     * [Markup.revision] is the document revision at which its own fields,
     * child list, or any descendant last changed, so an unchanged node keeps
     * its exact (id, revision) pair and a changed one carries the new
     * document's revision.
     *
     * Throws [ParseException] when the engine fails, and
     * [IllegalStateException] once this document has been [close]d or
     * superseded.
     */
    public fun edit(markdown: String): Document {
        val owned = mutableHandle()
        val carriedOptions = options
        val successor =
            decodeWire(owned.edit(markdown.encodeToByteArray())) {
                handle,
                id,
                revision,
                scope,
                content,
                index,
                diagnostics,
                ->
                Document(Built(handle, id, revision, scope, carriedOptions, content, diagnostics, index))
            }
        superseded.store(1L)
        return successor
    }

    /**
     * Hands this document trailing bytes and returns the document all bytes
     * so far describe, which SUPERSEDES this one.
     *
     * The streaming hot path. Any byte split is legal — mid-UTF-8, mid-CRLF,
     * mid-line — and an empty chunk still advances the chain, to an identical
     * tree. Appending never moves settled content: a node the append did not
     * reach keeps its id, its revision, and its positions, so the answer
     * arrives pruned — a subtree the encoder proved unchanged in both
     * content and position crosses the boundary as one reuse record and
     * resolves to the value this document already decoded, making the
     * per-append cost O(changed), not O(document).
     *
     * The successor is its chain's new head; this document keeps its decoded
     * values and its [close], and refuses further mutation deterministically.
     * A failed append past the argument guards poisons the chain: every
     * further mutation fails, the values and [close] of every document on
     * the chain remain, and recovery is a new [Document].
     *
     * Throws [ParseException] when the engine fails, and
     * [IllegalStateException] once this document has been [close]d or
     * superseded.
     */
    public fun append(chunk: String): Document {
        val owned = mutableHandle()
        val carriedOptions = options
        val successor =
            decodeWire(owned.append(chunk.encodeToByteArray(), built.revision), built.index) {
                handle,
                id,
                revision,
                scope,
                content,
                index,
                diagnostics,
                ->
                Document(Built(handle, id, revision, scope, carriedOptions, content, diagnostics, index))
            }
        superseded.store(1L)
        return successor
    }

    /**
     * This document's node for [id], or null when no node has that identity
     * here.
     *
     * An identity from another parse is null, not a failure: a caller
     * holding an id from a superseded revision is asking exactly this.
     */
    public fun node(id: MarkupID): Markup? =
        when {
            id.series != this.id.series -> null

            // The root answers for itself. It is a Markup like any other, and
            // its revision advances whenever the top-level block list changes,
            // so a consumer reconciling by id reaches this call with the
            // document's own id — while the index holds the descendants, not
            // the root.
            id.rawValue == this.id.rawValue -> this

            else -> built.index[id.rawValue]
        }

    /** Returns the canonical diagnostic dump for this document. */
    public fun dump(): String = MarkupDumper.dump(this)

    /** Returns the canonical diagnostic dump for the subtree rooted at
     * [node], with the subtree as scope origin. */
    public fun dump(node: Markup): String = MarkupDumper.dump(this, node)

    /**
     * Releases the native parse.
     *
     * Idempotent, and owed for every document a mutation superseded as much
     * as for the chain's head: superseding severs mutation, not ownership,
     * so a chain leaves one close per link — an O(1) release of a thin
     * shell for every superseded one. Every value this document already
     * produced — its content, scopes, diagnostics, and dump — stays usable
     * afterwards, because none of them borrow from the parse.
     */
    override fun close() {
        val owned = handle.exchange(0L)
        if (owned != 0L) {
            owned.release()
        }
    }
}
