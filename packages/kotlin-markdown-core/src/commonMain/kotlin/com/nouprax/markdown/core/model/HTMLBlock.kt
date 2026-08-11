@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

public class HTMLBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    /** True when the literal is one complete comment, so consumers without an
     * HTML parser can skip comment material by this bit alone. Asked of the
     * parser rather than derived here: the rule belongs to the engine, and a
     * copy in each binding is a second definition that can disagree. */
    public val comment: Boolean,
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
