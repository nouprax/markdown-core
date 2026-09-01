package com.nouprax.markdown.core

/**
 * The root of the semantic tree -- the view with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are
 * in no literal anywhere. It is an ordinary [Markup] node: nothing but its
 * [content] and its [scope], like every node under it. Its scopes are counted
 * against the normalized source (see [Read]), which the library does not hand
 * back.
 */
public class Semantic internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val id: Identity,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitSemantic(this)
}
