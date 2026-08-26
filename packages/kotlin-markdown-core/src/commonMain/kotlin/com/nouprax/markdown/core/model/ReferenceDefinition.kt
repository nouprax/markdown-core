package com.nouprax.markdown.core

/**
 * A link reference definition, at the byte where its opening bracket was
 * written. [label] is the bytes between the brackets exactly as the source
 * spells them; [norm] is the match key -- full Unicode case fold, trimmed,
 * internal whitespace collapsed -- and neither derives the other.
 * [destination] is never absent, because a definition that could not build one
 * is not produced at all; [title] is null when the source wrote none and empty
 * when it wrote an empty one.
 */
public class ReferenceDefinition internal constructor(
    public val label: String,
    public val norm: String,
    public val destination: String,
    public val title: String?,
    override val id: Identity,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitReferenceDefinition(this)
}
