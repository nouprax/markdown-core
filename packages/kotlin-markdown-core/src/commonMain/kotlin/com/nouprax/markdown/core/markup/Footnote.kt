package com.nouprax.markdown.core

/**
 * A footnote where it was written. A referenced definition `[^x]: body` is a
 * block in the content that holds it, with its normalized label and block
 * content; an inline note `^[body]` is owned by its citation's
 * [FootnoteTarget.Note], with a null label and direct inline content.
 * Duplicates and definitions nobody calls remain. [Document.footnotes] lists
 * every footnote in source order, and [Document.footnote] finds the first one
 * with a label.
 */
public class Footnote internal constructor(
    /** The normalized label without the caret, or null for an inline note. */
    public val label: String?,
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
