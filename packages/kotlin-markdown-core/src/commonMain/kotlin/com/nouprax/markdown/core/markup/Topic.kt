package com.nouprax.markdown.core

/** A `#name` topic: an inline leaf whose [name] excludes the `#`. */
public class Topic internal constructor(
    /** The name as written, without the `#`. */
    public val name: String,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
