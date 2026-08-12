package com.nouprax.markdown.core

/**
 * A footnote definition (`[^label]: …`), at the position it was written.
 *
 * - Never moved to the document tail.
 * - Never dropped when nothing references it.
 * - Never reordered by use.
 *
 * It carries no number, because a number is a rendering decision and the
 * tree does not make one.
 */
public class FootnoteDefinition internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The label between `[^` and `]`, exactly as written and not
     * normalized.
     *
     * The engine pairs a reference with a definition case-folded, trimmed,
     * and with runs of inner whitespace collapsed to one space, so comparing
     * two of these strings byte for byte is a stricter test than the one
     * that matched them. */
    public val label: String,
    /** The definition's block content in source order. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/**
 * A reference (`[^label]`) that resolves to a footnote definition.
 *
 * A reference with no definition is not one: it stays the literal text the
 * author typed, and that text is not reparsed — `[^~~x~~]` with nothing
 * defining it is one [Text] holding no [Strikethrough]. A consumer never
 * meets an unresolvable reference node.
 */
public class FootnoteReference internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The label this reference resolves by, exactly as written and matched
     * the way [FootnoteDefinition.label] describes. */
    public val label: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
