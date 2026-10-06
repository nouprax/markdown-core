package com.nouprax.markdown.core

public class Directive internal constructor(
    public val name: String,
    /** Markup owned by the label field, not a generic child/content element. */
    public val label: DirectiveLabel?,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
