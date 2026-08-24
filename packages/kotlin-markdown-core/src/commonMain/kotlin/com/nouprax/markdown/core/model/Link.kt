package com.nouprax.markdown.core

public class Link internal constructor(
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer `""`. A link with no destination at all is a
     * [LinkReference].
     */
    public val destination: String,
    /** Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one. */
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitLink(this)
}
