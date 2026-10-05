package com.nouprax.markdown.core

public class ThematicBreak internal constructor(
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
