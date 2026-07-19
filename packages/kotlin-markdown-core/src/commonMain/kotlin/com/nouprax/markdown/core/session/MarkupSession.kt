package com.nouprax.markdown.core

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * The single mutable owner of one Markdown text and its living AST.
 *
 * Queue edits ([append] is an edit at end-of-text), then [commit]: the
 * session reparses incrementally, keeps node identity wherever content is
 * unchanged, and returns the new snapshot with the exact delta. After any
 * sequence of edits and commits the document is semantically identical to a
 * one-shot [Document.parse] of the same final text.
 *
 * Calls on one session must be externally synchronized (one writer at a
 * time); snapshots are immutable values that survive the session. Commits
 * are transactional: on failure the session stays valid at its previous
 * revision and the commit may be retried (applied edits are retained — the
 * text advances, the tree does not). [close] releases the native session;
 * snapshots, deltas, and materialized scopes remain usable afterwards.
 */
public class MarkupSession(
    public val options: ParseOptions = ParseOptions(),
) : AutoCloseable {
    internal val native: CSession = CSession(options)

    /**
     * Per-session random salt; nodes from different sessions never compare
     * equal even when their raw ids collide numerically.
     */
    public val lineage: ULong = native.lineage()

    internal val mirror: HashMap<ULong, Markup> = HashMap()
    private val rootId: ULong = native.rootId()
    private var resolver: ScopeResolver
    private var closed = false
    private var failed = false

    /**
     * The last committed snapshot; the empty document at revision 0 until
     * the first commit.
     */
    public var document: Document
        private set

    init {
        val resolver = ScopeResolver.live(native)
        this.resolver = resolver
        // The revision-0 root is always an empty document.
        val root = Document(MarkupID(lineage, rootId), 0UL, emptyList(), resolver)
        mirror[rootId] = root
        document = root
    }

    /** The revision of the last committed snapshot; 0 before the first
     * commit. */
    public val revision: ULong
        get() {
            requireOpen()
            return native.revision()
        }

    /** The byte length of the stored text, including uncommitted edits. */
    public val length: Int
        get() {
            requireOpen()
            return native.length().toInt()
        }

    /**
     * Queues an append of [text]'s UTF-8 bytes at the end of the stored
     * text. Nothing is parsed until [commit].
     */
    public fun append(text: String) {
        requireOpen()
        val end = native.length()
        edit(end, end, text)
    }

    /**
     * Queues a replacement of the byte range `[start, end)` of the stored
     * text with [replacement]'s UTF-8 bytes. An empty range inserts; an
     * empty [replacement] deletes. Offsets refer to the stored text as
     * previously edited; nothing is parsed until [commit].
     */
    public fun replace(
        start: Int,
        end: Int,
        replacement: String,
    ) {
        requireOpen()
        require(start in 0..end) { "invalid edit range [$start, $end)" }
        edit(start.toLong(), end.toLong(), replacement)
    }

    private fun edit(
        start: Long,
        end: Long,
        replacement: String,
    ) {
        WireDecoder.decodeAck(native.edit(start, end, replacement.encodeToByteArray()))
    }

    /**
     * Reparses the pending text incrementally and returns the new snapshot
     * with its delta. The snapshot shares every unchanged node value with
     * the previous snapshot; the work is proportional to the delta, not the
     * document.
     */
    public fun commit(): Commit {
        requireOpen()
        // The previous snapshot's currency ends when the commit starts:
        // detach its resolver before the native tree is replaced, so a
        // not-yet-materialized snapshot can never cache the new revision's
        // positions as its own — a racing reader either materialized from
        // the still-unchanged tree or takes the documented
        // superseded-snapshot failure.
        val previous = resolver
        previous.detach()
        val changes =
            try {
                WireDecoder.decodeCommit(native.commit(), lineage, mirror)
            } catch (failure: ParseException) {
                // The native commit failed transactionally: the tree is
                // unchanged at the previous revision, the previous snapshot
                // becomes current again, and the commit may be retried.
                previous.reattach(native)
                throw failure
            } catch (failure: Throwable) {
                // The native tree may have advanced while the payload or the
                // mirror did not; the session can no longer answer
                // consistently and refuses further work.
                failed = true
                throw failure
            }
        val resolver = ScopeResolver.live(native)
        this.resolver = resolver
        val root = mirror[rootId]
        check(root is Document) { "session committed without a document root" }
        val adopted = Document(root.id, root.revision, root.content, resolver)
        mirror[rootId] = adopted
        document = adopted
        return Commit(adopted, changes)
    }

    /**
     * The committed snapshot's current value for [id]; null when no node
     * with that identity exists at the committed revision.
     */
    public fun node(id: MarkupID): Markup? = if (id.lineage == lineage) mirror[id.rawValue] else null

    /**
     * Async sugar over the streaming hot path: appends each token from
     * [input] and commits, yielding one [Commit] per token. Collect on the
     * context that owns the session; coalescing tokens before feeding them
     * trades latency for throughput exactly as manual [append] + [commit]
     * does.
     */
    public fun updates(input: Flow<String>): Flow<Commit> =
        flow {
            input.collect { token ->
                append(token)
                emit(commit())
            }
        }

    /**
     * Releases the native session. Idempotent. Snapshots, deltas, and scopes
     * materialized while their snapshot was current remain usable; every
     * other member of this class fails after closing.
     */
    override fun close() {
        if (closed) {
            return
        }
        closed = true
        resolver.detach()
        native.free()
    }

    internal fun requireOpen() {
        check(!closed) { "the session is closed" }
        check(!failed) { "the session failed irrecoverably during a commit" }
    }
}
