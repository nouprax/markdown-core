package com.nouprax.markdown.core

public class Subscript internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
