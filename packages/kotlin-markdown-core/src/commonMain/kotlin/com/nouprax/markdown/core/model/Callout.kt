package com.nouprax.markdown.core

/**
 * A callout: every `>` container. A plain quoted block is a callout without
 * metadata, with a null [variant], [CalloutFold.NONE], and a null [title];
 * the callouts module's metadata rule, which fills them in, lands with `O8`.
 */
public class Callout internal constructor(
    /** The authored type as written, or null when the container has no metadata line. */
    public val variant: String?,
    public val fold: CalloutFold,
    /** The title's inline content, owned by the callout and never an element of [content]; null when no title was authored. */
    public val title: kotlin.collections.List<Markup>?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitCallout(this)
}
