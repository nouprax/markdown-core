package com.nouprax.markdown.core

public class Definition internal constructor(
    public val term: kotlin.collections.List<Markup>,
    public val content: kotlin.collections.List<kotlin.collections.List<Markup>>,
    public val compact: Boolean,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
