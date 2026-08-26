package com.nouprax.markdown.core

/**
 * The root of the semantic tree -- the view with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are
 * in no literal anywhere. It is an ordinary [Markup] node: nothing but its
 * [content] and its [scope], like every node under it. What it does NOT carry
 * is the text its scopes are counted against -- a root detached from its
 * [Concrete] is not self-interpreting, which is why the two travel together
 * as a [Read] and never alone.
 */
public class Semantic internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitSemantic(this)
}
