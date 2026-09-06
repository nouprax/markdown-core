package com.nouprax.markdown.core

public class Image internal constructor(
    /** Required, for the reason [Link.dest] is. */
    public val dest: Destination,
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitImage(this)
}
