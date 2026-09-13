package com.nouprax.markdown.core

/** A document-owned Markup definition with authored label and counter-reset facts. */
public class Specimen internal constructor(
    /** The authored label, or null for an anonymous definition. */
    public val id: String?,
    /** An explicit counter reset, or null when numbering continues. */
    public val start: Long?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String? = null,
    override val attributes: Attributes = Attributes.empty,
) : Markup {
    override fun <Result> accept(
        visitor: Visitor<Result>,
        phase: MarkupWalkPhase,
    ): Result = visitor.visit(this, phase)
}
