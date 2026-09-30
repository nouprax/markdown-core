package com.nouprax.markdown.core

public class LineBreak internal constructor(
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
