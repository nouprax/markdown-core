package com.nouprax.markdown.core

/** An inline formula (the math extension). */
public class Formula internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Which delimiters were written.
     *
     * - `$…$` and `\\(…\\)` are [PlacementMode.EMBEDDED]
     * - `$$…$$` and `\\[…\\]` are [PlacementMode.STANDALONE]
     *
     * Placement is not containment — a display formula written mid-paragraph
     * is standalone and still sits inside that paragraph's inline content. */
    public val mode: PlacementMode,
    /** The formula source between the delimiters, left undecoded.
     *
     * A math renderer wants the bytes the author typed, so backslash escapes
     * and character references stay as written. The one exception is inside
     * the `\\(…\\)` and `\\[…\\]` forms, where an escaped closing delimiter
     * resolves — `\)` there is a `)` here. One space or line ending is
     * dropped from each end when the content both begins and ends with one
     * and is not entirely spaces, the same padding rule a code span
     * follows. */
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
