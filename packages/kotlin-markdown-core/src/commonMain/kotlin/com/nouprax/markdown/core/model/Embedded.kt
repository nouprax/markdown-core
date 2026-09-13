package com.nouprax.markdown.core

/**
 * An inline embed from direct or resolved Markdown image syntax. The target type is not inferred.
 * Complete `W`, `WxH`, `alt|W` and `alt|WxH` labels
 * supply positive 32-bit dimensions without leading zeros.
 */
public class Embedded internal constructor(
    /** Required, for the reason [Link.dest] is. */
    public val dest: Destination,
    public val title: String?,
    /** Authored size from a complete label suffix, or null. Independent of attribute records. */
    public val dimensions: Dimensions?,
    /** Parsed alt excluding a valid suffix; numeric-only labels are empty, malformed suffixes remain. */
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
