package com.nouprax.markdown.core

import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.ExperimentalAtomicApi

/**
 * The syntactic context at a cursor: the open state of a parse of the text
 * before it, confirmed against the document parsed from the whole text.
 *
 * A context lists the blocks the text before the cursor leaves open, the
 * inline constructs it leaves open in the inline content that holds the
 * cursor, and the fields an open construct reads at the cursor, which the
 * whole document confirms. [MarkdownSession.context] asks one.
 */
public class Context internal constructor(
    /**
     * The blocks the text before the cursor leaves open, outermost first,
     * each with the scope of its opening line up to the cursor.
     */
    public val blocks: kotlin.collections.List<Opener>,
    /**
     * The constructs the text before the cursor leaves open in the inline
     * content that holds it, earliest first, each with the scope of its
     * opening syntax.
     */
    public val inlines: kotlin.collections.List<Opener>,
    /**
     * The fields the cursor may be writing. There is more than one only where
     * the grammar itself leaves the text before the cursor ambiguous.
     */
    public val slots: kotlin.collections.List<Slot>,
)

/** A construct a context finds open. */
@ConsistentCopyVisibility
public data class Opener internal constructor(
    /**
     * The kind it opens, or null while that is still open too: a `[` may open
     * a link, a span or a cite.
     */
    public val kind: NodeKind?,
    /** Its opening syntax, or for a block its opening line up to the cursor. */
    public val scope: Scope,
)

/**
 * A field of kind [kind] of a construct of kind [owner] that the cursor may
 * be writing.
 *
 * A slot has no value equality: how it spells depends on the syntax of the
 * text it was read from, such as the `<...>` around a destination, which its
 * fields do not show.
 */
public class Slot internal constructor(
    /** The field's role. */
    public val kind: SlotKind,
    /** The construct the field belongs to. */
    public val owner: NodeKind,
    /**
     * The field's source from its start to the cursor: what the user has
     * written of it. It does not leave the cursor's line.
     */
    public val before: Scope,
    /**
     * The field's continuation from the cursor, as the construct's own
     * grammar reads it. It does not leave the cursor's line.
     */
    public val after: Scope,
    /**
     * Whether the whole document finishes the construct after the cursor;
     * otherwise it reads the construct's opener as text.
     */
    public val closed: Boolean,
    /**
     * The earlier fields the slot depends on, as written, such as the note
     * before a cross link's anchor.
     */
    public val prefix: kotlin.collections.List<String>,
    /** The engine's context the slot was read from, which a spelling asks. */
    private val handle: ContextHandle,
    /** The slot's place among its context's slots. */
    private val index: Int,
) {
    /** What replaces a slot's [before] and [after] to write a value in it. */
    @ConsistentCopyVisibility
    public data class Insertion internal constructor(
        /**
         * The value in the field's own syntax, and the shortest closer that
         * finishes the construct when it is not [closed].
         */
        public val text: String,
        /**
         * Where the caret lands: the columns of [text] before it, in the
         * session's [TextUnit]. It stands after the value, before any closer.
         */
        public val cursor: Int,
    )

    /**
     * The parser's spelling of each of [values] in this slot, in order.
     *
     * A spelling writes the value in the field's own syntax: it escapes what
     * the field reads as syntax, and writes in `<...>`, in braces or in quotes
     * a value whose bare spelling the field does not read whole. It is null
     * for a value the field holds in no spelling there, such as a line
     * ending, a `|` in a cross target or a space in a key.
     *
     * The slot keeps the engine's context it was read from, so a spelling
     * parses nothing; it stays the spelling of the text the context was asked
     * of after the session moves on, until the session closes.
     *
     * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when an
     *   allocation fails.
     * @throws IllegalStateException when the session is closed.
     */
    public fun spell(values: kotlin.collections.List<String>): kotlin.collections.List<Insertion?> =
        values.map { handle.session.spell(handle.address, index, it) }

    /** The parser's spelling of [value] in this slot, as the list form spells each value. */
    public fun spell(value: String): Insertion? = spell(listOf(value))[0]

    public companion object {
        /**
         * The characters after which a slot may begin with nothing written
         * yet, in ascending order: the last character of a slot's opener, such
         * as the second `[` of `[[` or the `(` of `](`; a separator before a
         * field, such as the space before an attribute record; and a line
         * ending, before a properties member's name. An editor that asks for
         * completions when one is typed asks wherever a slot opens empty.
         */
        public val cues: kotlin.collections.List<Char> by lazy { dialect.second }
    }
}

/**
 * An engine's context that a session made, which the slots read from it
 * spell from. It is freed once no slot can reach it, or when the session
 * closes, whichever comes first.
 */
internal class ContextHandle(
    val session: MarkdownSession,
    val native: NativeContext,
) {
    val address: Long get() = native.address

    /** What frees [native] once the handle is unreachable, which it keeps. */
    private var watch: Any? = null

    /** Frees [native] once no slot can reach the handle. */
    fun watch() {
        watch = reclaim(this, native::free)
    }
}

/**
 * The engine's context at [address], freed once by whichever of its
 * handle's reclaim and its session's close comes first, on any thread.
 */
@OptIn(ExperimentalAtomicApi::class)
internal class NativeContext(
    private val platform: PlatformSession,
    val address: Long,
) {
    private val freed = AtomicBoolean(false)

    /** Whether the context is freed. */
    val isFreed: Boolean get() = freed.load()

    /** Frees the context; a second call, or a call for no context, does nothing. */
    fun free() {
        if (address != 0L && freed.compareAndSet(false, true)) platform.release(address)
    }
}

/** The metadata field names and the slot cues, read from the engine once. */
internal val dialect: Pair<kotlin.collections.List<String>, kotlin.collections.List<Char>> by lazy {
    AnswerDecoder.dialect(platformDialect())
}
