package com.nouprax.markdown.core

public class Link internal constructor(
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer a [Destination.Url] holding `""`. A reference
     * occurrence answers a [Destination.Reference] naming its definition.
     */
    public val dest: Destination,
    /**
     * Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one. A
     * reference occurrence writes none; the [Reference] it names states its own.
     */
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
