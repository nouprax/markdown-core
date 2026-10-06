package com.nouprax.markdown.core

public class DefinitionList internal constructor(
    public val definitions: kotlin.collections.List<Definition>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
