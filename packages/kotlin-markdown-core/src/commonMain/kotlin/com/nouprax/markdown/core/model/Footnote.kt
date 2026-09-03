package com.nouprax.markdown.core

/**
 * A footnote definition. [identifier] KEEPS the leading `^` that [label] does
 * not carry, so a footnote and a link definition of one name cannot collide in
 * a consumer's single map.
 */
public class FootnoteDefinition internal constructor(
    public val label: String,
    public val identifier: String,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteDefinition(this)
}

/** A footnote call. There is one footnote syntax, so it carries no form. */
public class FootnoteReference internal constructor(
    public val label: String,
    public val identifier: String,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteReference(this)
}
