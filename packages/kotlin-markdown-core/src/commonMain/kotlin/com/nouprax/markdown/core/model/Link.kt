package com.nouprax.markdown.core

/**
 * A hyperlink whose content is its inline caption.
 *
 * An autolink (`<https://example.com>`) arrives as a Link too, its caption
 * the single text node holding the URL.
 */
public class Link internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The destination with backslash escapes and character references
     * already resolved, so it is not always the spelling [scope] covers.
     *
     * Empty when the link writes empty parentheses, as `[text]()` and
     * `[text](<>)` both do. Never null: an inline link always writes its
     * `(…)`, so there is no unwritten case to tell an empty one from. */
    public val destination: String,
    /** The title in quotes after the destination, its quotes dropped and the
     * same unescaping applied.
     *
     * Null when none is written, the empty string when one is written empty:
     * `[a](/u)` gives null and `[a](/u "")` gives `""`. An autolink writes no
     * title, so it gives null like any other link. */
    public val title: String?,
    /** The link's inline caption content in source order. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
