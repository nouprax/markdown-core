package com.nouprax.markdown.core

/** An inline code span. */
public class Code internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Always [PlacementMode.EMBEDDED]: a code span is inline by definition.
     *
     * The field is here because placement is one question asked of the code,
     * formula, and directive kinds alike, and [Formula] is the only one whose
     * answer varies. */
    public val mode: PlacementMode,
    /** The span's text after normalization.
     *
     * Each line ending inside it becomes one space, and one space is dropped
     * from each end when the content has both and is not entirely spaces. It
     * is therefore not always the source between the backticks. */
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
