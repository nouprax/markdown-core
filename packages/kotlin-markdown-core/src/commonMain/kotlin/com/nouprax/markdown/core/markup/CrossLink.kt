package com.nouprax.markdown.core

/** A workspace link. [label] is absent only when no separator was written. */
public class CrossLink internal constructor(
    public val dest: Destination,
    /** The complete raw authored label. */
    public val label: String?,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
