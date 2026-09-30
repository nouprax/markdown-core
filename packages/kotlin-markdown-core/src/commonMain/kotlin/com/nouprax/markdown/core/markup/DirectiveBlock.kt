package com.nouprax.markdown.core

public class DirectiveBlock internal constructor(
    public val name: String?,
    /** Markup owned by the label field, never an element of [content]. */
    public val label: DirectiveLabel?,
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
