package com.nouprax.markdown.core

/**
 * A callout: every `>` container. A plain quoted block is a callout without
 * metadata, with a null [variant], a null [collapsed], and a null [title];
 * a valid opening `[!type]` line populates metadata with the type as written.
 */
public class Callout internal constructor(
    /** The authored type as written, or null when the container has no metadata line. */
    public val variant: String?,
    /** The fold marker: null when no `+` or `-` was authored, false for `+`, which opens expanded, and true for `-`. */
    public val collapsed: Boolean?,
    /** The title's inline content, owned by the callout and never an element of [content]; null when no title was authored, and never empty. */
    public val title: kotlin.collections.List<Markup>?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitCallout(this)
}
