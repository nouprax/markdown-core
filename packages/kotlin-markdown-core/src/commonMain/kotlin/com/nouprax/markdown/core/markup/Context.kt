package com.nouprax.markdown.core

/**
 * The syntactic context at a cursor: the open state of a parse of the text
 * before it, confirmed against the document parsed from the whole text.
 *
 * A context lists the blocks the text before the cursor leaves open, the
 * inline constructs it leaves open in the inline content that holds the
 * cursor, and the fields an open construct reads at the cursor, which the
 * whole document confirms.
 *
 * A session keeps its context as its state at its cursor:
 * [MarkdownSession.locate] moves the cursor and updates
 * [MarkdownSession.context], and [MarkdownSession.spell] spells from that
 * state as it stands.
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

/** The context of a session not yet located: nothing is open. */
internal val blank = Context(emptyList(), emptyList(), emptyList())

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
 * A slot names a field of its session's [MarkdownSession.context] as the
 * locate that read it left it: it spells only while that context stands. It
 * has no value equality: how it spells depends on the syntax of the text it
 * was read from, such as the `<...>` around a destination, which its fields
 * do not show.
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
    /** The slot's place among its context's slots. */
    internal val index: Int,
    /** The session's turn whose context holds the slot. */
    internal val turn: Int,
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

/** The metadata field names and the slot cues, read from the engine once. */
internal val dialect: Pair<kotlin.collections.List<String>, kotlin.collections.List<Char>> by lazy {
    AnswerDecoder.dialect(platformDialect())
}
