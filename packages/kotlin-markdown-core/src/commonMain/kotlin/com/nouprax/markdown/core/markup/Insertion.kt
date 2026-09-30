package com.nouprax.markdown.core

public class Insertion internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
