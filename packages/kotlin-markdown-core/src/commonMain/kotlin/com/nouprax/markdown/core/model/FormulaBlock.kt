package com.nouprax.markdown.core

/**
 * A standalone formula block (the math extension): `$$…$$`, `\\[…\\]`, or a
 * fenced block whose info string is `formula`.
 */
public class FormulaBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Always [PlacementMode.STANDALONE] — the engine refuses any other mode
     * on a block.
     *
     * The inline [Formula] is the one place the answer varies. */
    public val mode: PlacementMode,
    /** The formula source between the delimiters, left undecoded.
     *
     * Backslash escapes and character references stay as the author typed
     * them. The delimiters and the line breaks beside them are off it, and
     * unlike [CodeBlock.literal] it does not end with a newline. */
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
