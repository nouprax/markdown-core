@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

/** A block of raw HTML, passed through unparsed. */
public class HTMLBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** True when the literal is one complete comment, so consumers without an
     * HTML parser can skip comment material by this bit alone.
     *
     * Asked of the parser rather than derived here: the rule belongs to the
     * engine, and a copy in each binding is a second definition that can
     * disagree. */
    public val comment: Boolean,
    /** The HTML as written.
     *
     * It always ends with a newline: the parser supplies one for a block that
     * ran to the end of the input without it. */
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
