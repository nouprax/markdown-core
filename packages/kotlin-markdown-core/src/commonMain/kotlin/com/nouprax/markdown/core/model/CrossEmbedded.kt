package com.nouprax.markdown.core

/** A workspace transclusion. [label] is absent only when no separator was written. */
public class CrossEmbedded internal constructor(
    public val dest: Destination,
    /** Raw prefix after a valid size suffix. */
    public val label: String?,
    /** Authored dimensions, absent for an invalid or missing suffix. */
    public val dimensions: Dimensions?,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    init {
        require(dimensions == null || label != null) { "dimensions require an authored label" }
    }

    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
