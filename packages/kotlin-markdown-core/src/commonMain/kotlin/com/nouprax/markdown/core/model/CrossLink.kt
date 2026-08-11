@file:kotlin.jvm.JvmName("CrossLinkKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

/** A cross-link written as `[[reference]]`. */
public class CrossLink(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The source-faithful reference between the delimiters. */
    public val reference: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
