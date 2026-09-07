package com.nouprax.markdown.core

/** An authored workspace reference. [label] is absent only when no separator was written. */
public class CrossLink internal constructor(
    public val embedded: Boolean,
    public val dest: Destination,
    public val label: String?,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitCrossLink(this)
}
