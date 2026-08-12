package com.nouprax.markdown.core

/** An indented or fenced code block. */
public class CodeBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Always [PlacementMode.STANDALONE].
     *
     * The kind has no other legal placement, so the binding fills it in
     * without asking the parser. */
    public val mode: PlacementMode,
    /** The whole info string written after the opening fence.
     *
     * Null for an indented block, and for a fence with nothing after it. */
    public val info: String?,
    /** The first word of [info] — the conventional language tag. */
    public val language: String?,
    /** The code itself: the fences and the block's own indentation are
     * already off it.
     *
     * Non-empty text ends with a newline even where the source stopped
     * without one. */
    public val literal: String,
    public val fenced: Boolean,
    /** Whether a fenced block's closing fence was there.
     *
     * False for a fence that runs to the end of the input — what a document
     * being typed looks like — and true for an indented block, which has no
     * fence to leave open. */
    public val closed: Boolean,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
