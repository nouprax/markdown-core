package com.nouprax.markdown.core

/** A workspace link. [label] is absent only when no separator was written. */
public class CrossLink internal constructor(
    public val dest: Destination,
    /** The complete raw authored label. */
    public val label: String?,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
