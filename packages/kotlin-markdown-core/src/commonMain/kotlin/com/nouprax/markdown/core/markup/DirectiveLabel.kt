package com.nouprax.markdown.core

/**
 * A directive's bracketed label. It is Markup owned by the directive's label
 * field, not directive content. Its scope spans the brackets, so a label
 * written empty is still a place in the source.
 */
public class DirectiveLabel internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
