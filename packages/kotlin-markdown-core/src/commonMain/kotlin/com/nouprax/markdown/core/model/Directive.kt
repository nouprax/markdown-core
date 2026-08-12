package com.nouprax.markdown.core

public class Directive internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    public val mode: PlacementMode,
    public val name: String,
    /** The directive's attribute map in source order, or null when no
     * `{...}` container was written; an empty container is an empty map. */
    public val attributes: Map<String, String>?,
    public val label: DirectiveLabel?,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
