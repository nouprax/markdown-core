package com.nouprax.markdown.core

/** Which of the three reference spellings the source wrote. */
public enum class ReferenceForm {
    FULL,
    COLLAPSED,
    SHORTCUT,
}

/**
 * A link reference. It carries NO destination: the destination is stated once,
 * at the definition, and [identifier] is what names it. [form] records which of
 * `[text][label]`, `[label][]` and `[label]` the source wrote; all three
 * resolve identically, so nothing else on the node recovers it.
 */
public class LinkReference internal constructor(
    public val label: String,
    public val identifier: String,
    public val form: ReferenceForm,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitLinkReference(this)
}

/** An image reference. As [LinkReference]; the content is parsed alt text. */
public class ImageReference internal constructor(
    public val label: String,
    public val identifier: String,
    public val form: ReferenceForm,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitImageReference(this)
}
