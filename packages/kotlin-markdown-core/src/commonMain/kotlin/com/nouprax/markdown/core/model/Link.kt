package com.nouprax.markdown.core

public class Link internal constructor(
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer a [Destination.Url] holding `""`. A reference
     * occurrence answers the destination its definition stated.
     */
    public val dest: Destination,
    /** Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one. */
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
