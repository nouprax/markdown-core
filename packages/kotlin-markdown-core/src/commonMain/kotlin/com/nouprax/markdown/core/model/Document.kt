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
    decodeWireOpen(cOpen(markdown.encodeToByteArray(), options)) {
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
 *     val commit = document.edit("# Renamed")
 * }
 * ```
 *
 * There is no session type. A document is created from text and options;
 * [edit] hands it new text and returns the next document with the delta
 * between them. Options are fixed for a document's whole series — changing
 * what the parser means is a new `Document`, not an edit, and there is no
 * delta to be had across it.
 *
 * The node values are ordinary immutable values with no reference back here:
 * hold them, put them in a view model, hand them to another thread. What this
 * class owns is the native parse, which [edit] needs in order to keep
 * identities stable across revisions — and which [close] releases. A document
 * that is neither closed nor edited is released once it becomes unreachable,
 * but that is a backstop, not the contract.
 */
public class Document private constructor(
    private val built: Built,
) : Markup,
    AutoCloseable {
    /** Parses [markdown]. */
    @JvmOverloads
    public constructor(markdown: String, options: ParseOptions = ParseOptions()) : this(open(markdown, options))

    /** The native parse, or zero once something has taken it. Both paths that
     * END the parse — [close] and the platform's reclaim — go through this
     * one exchange, and only the first wins. [edit] is not one of them: it
     * READS the parse and leaves it here. */
    private val handle = AtomicLong(built.handle)

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

    override val scope: Scope get() = built.scope

    /** The options this document and its whole series were parsed under. */
    public val options: ParseOptions get() = built.options

    /** The document's top-level blocks in source order. */
    public val content: kotlin.collections.List<Markup> get() = built.content

    /** Everything an editor should underline, in source order. Empty for
     * almost every document; see [DiagnosticCode]. */
    public val diagnostics: kotlin.collections.List<Diagnostic> get() = built.diagnostics

    /**
     * Per-series random salt; nodes from different parses never compare equal
     * even when their raw ids collide numerically.
     */
    public val series: ULong get() = id.series

    /** [series] as a bit-preserving signed value: the Java view of the
     * unsigned accessor, whose mangled name Java sources cannot write. */
    public fun seriesBits(): Long = series.toLong()

    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)

    /**
     * Hands this document new text and returns the document that text
     * describes, together with what changed.
     *
     * Reads the receiver and takes nothing from it: this document stays
     * usable and may be edited again. Editing it twice gives two lines of
     * descent, told apart by their revisions — and, like nodes from two
     * separate parses, nodes from two lines are not comparable.
     */
    public fun edit(markdown: String): Commit {
        val owned = handle.load()
        check(owned != 0L) { "the document is closed" }
        val carriedOptions = options
        val (successor, delta) =
            decodeWireEdit(owned.edit(markdown.encodeToByteArray())) {
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
        return Commit(successor, delta)
    }

    /**
     * This document's node for [id], or null when no node has that identity
     * here. An identity from another parse is null, not a failure: a caller
     * holding an id from a superseded revision is asking exactly this.
     */
    public fun node(id: MarkupID): Markup? =
        when {
            id.series != this.id.series -> null

            // The root answers for itself. It is a Markup like any other, and
            // a delta names it whenever the top-level block list changes, so a
            // consumer reconciling by id reaches this call with the document's
            // own id — while the mirror holds the descendants, not the root.
            id.rawValue == this.id.rawValue -> this

            else -> built.index[id.rawValue]
        }

    /** Returns the canonical diagnostic dump for this document. */
    public fun dump(): String = MarkupDumper.dump(this)

    /** Returns the canonical diagnostic dump for the subtree rooted at
     * [node], with the subtree as scope origin. */
    public fun dump(node: Markup): String = MarkupDumper.dump(this, node)

    /**
     * Releases the native parse. Idempotent, and unnecessary after [edit],
     * which hands the parse to the successor. Every value this document
     * already produced — its content, scopes, diagnostics, and dump — stays
     * usable afterwards, because none of them borrow from the parse.
     */
    override fun close() {
        val owned = handle.exchange(0L)
        if (owned != 0L) {
            owned.release()
        }
    }
}
