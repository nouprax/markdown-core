package com.nouprax.markdown.core

/**
 * How a reference was written. The three resolve identically and differ only
 * in source form, which the tree keeps because it is what was written.
 */
public enum class ReferenceForm {
    FULL,
    COLLAPSED,
    SHORTCUT,
}

/**
 * A link reference definition, at the position it was written. The
 * destination is stated here, once, rather than copied into every reference
 * that resolves to it.
 */
public class ReferenceDefinition internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val label: String,
    public val destination: String?,
    public val title: String?,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/**
 * `[text][label]`, `[label][]`, or `[label]`. It carries no destination:
 * which definition the label resolves to is an answer, asked of the document
 * rather than read off the node. [Link] stays the inline form `[a](/u)`,
 * whose destination is written in the source.
 */
public class LinkReference internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val label: String,
    public val form: ReferenceForm,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/** `![alt][label]` and its collapsed and shortcut forms. */
public class ImageReference internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val label: String,
    public val form: ReferenceForm,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
