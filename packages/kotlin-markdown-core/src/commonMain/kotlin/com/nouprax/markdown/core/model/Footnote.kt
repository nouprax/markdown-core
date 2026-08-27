package com.nouprax.markdown.core

/**
 * A footnote definition. [norm] is the match key the label folds to, and it
 * KEEPS the leading `^` that [label] does not carry, so a footnote and a link
 * definition of one name cannot collide in a consumer's single map.
 */
public class FootnoteDefinition internal constructor(
    public val label: String,
    public val norm: String,
    public val content: kotlin.collections.List<Markup>,
    override val id: Identity,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteDefinition(this)
}

/**
 * A footnote call. There is one footnote syntax, so it carries no form.
 * [definition] is the identity of the [FootnoteDefinition] it resolved to --
 * the first definition of its label in document order.
 */
public class FootnoteReference internal constructor(
    public val label: String,
    public val definition: Identity,
    override val id: Identity,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteReference(this)
}
