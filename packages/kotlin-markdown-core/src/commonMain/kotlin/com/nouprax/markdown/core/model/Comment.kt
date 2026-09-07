package com.nouprax.markdown.core

/**
 * A comment: an inline HTML comment token, or an HTML block that opened with
 * `<!--` and closed on a `-->` line. The one kind valid in both block and
 * inline content; the parent records which. [literal] is the bytes between the
 * delimiters, exactly as written.
 */
public class Comment internal constructor(
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitComment(this)
}
