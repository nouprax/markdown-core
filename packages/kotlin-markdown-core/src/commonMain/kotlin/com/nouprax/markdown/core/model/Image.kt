package com.nouprax.markdown.core

/** An image whose content is its inline description. */
public class Image internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The source with backslash escapes and character references already
     * resolved, so it is not always the spelling [scope] covers.
     *
     * Empty when the image writes empty parentheses, as `![alt]()` does.
     * Never null: an inline image always writes its `(…)`, so there is no
     * unwritten case to tell an empty one from. */
    public val source: String,
    /** The title in quotes after the source, its quotes dropped and the same
     * unescaping applied; null when the image states none. */
    public val title: String?,
    /** The image's alt text, kept as parsed inline content in source order
     * rather than flattened to a string. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
