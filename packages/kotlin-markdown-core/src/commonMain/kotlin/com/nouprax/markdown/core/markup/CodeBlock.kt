package com.nouprax.markdown.core

public class CodeBlock internal constructor(
    public val info: String?,
    public val language: String?,
    public val literal: String,
    public val fenced: Boolean,
    public val closed: Boolean,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
