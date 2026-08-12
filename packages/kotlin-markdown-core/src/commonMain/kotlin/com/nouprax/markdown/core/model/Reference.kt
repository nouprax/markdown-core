package com.nouprax.markdown.core

/**
 * How a reference was written.
 *
 * The three resolve identically and differ only in source form, which the
 * tree keeps because it is what was written.
 */
public enum class ReferenceForm {
    /** `[text][label]`. */
    FULL,

    /** `[label][]`. */
    COLLAPSED,

    /** `[label]`. */
    SHORTCUT,
}

/**
 * A link reference definition, at the position it was written.
 *
 * The destination is stated here, once, rather than copied into every
 * reference that resolves to it.
 */
public class ReferenceDefinition internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The label between `[` and `]`, exactly as written and not normalized.
     *
     * The engine pairs a reference with a definition case-folded, trimmed,
     * and with runs of inner whitespace collapsed to one space, so comparing
     * two of these strings byte for byte is a stricter test than the one
     * that matched them. */
    public val label: String,
    /** The destination this definition assigns to its label.
     *
     * Empty when written empty, as `[foo]: <>` does. Never null: a
     * definition that writes no destination is not a definition at all. */
    public val destination: String,
    public val title: String?,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/**
 * `[text][label]`, `[label][]`, or `[label]`.
 *
 * It carries no destination: which definition the label resolves to is an
 * answer, asked of the document rather than read off the node. [Link] stays
 * the inline form `[a](/u)`, whose destination is written in the source.
 */
public class LinkReference internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The label this reference resolves by, exactly as written and matched
     * the way [ReferenceDefinition.label] describes. */
    public val label: String,
    public val form: ReferenceForm,
    /** The reference's inline caption content in source order.
     *
     * In the collapsed and shortcut forms the parser fills it from the
     * label's own text, parsed as inline markup, so a renderer never
     * synthesizes one. */
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
    override val scope: Scope,
    /** The label this reference resolves by, exactly as written and matched
     * the way [ReferenceDefinition.label] describes. */
    public val label: String,
    public val form: ReferenceForm,
    /** The reference's alternative-text content in source order.
     *
     * In the collapsed and shortcut forms the parser fills it from the
     * label's own text, parsed as inline markup. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
