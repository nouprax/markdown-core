package com.nouprax.markdown.core

/**
 * A specimen definition where it was written, with its authored label and
 * counter-reset facts. [Document.specimens] lists every specimen in source
 * order, and [Document.specimen] finds the first one with a label.
 */
public class Specimen internal constructor(
    /** The authored label, or null for an anonymous definition. */
    public val label: String?,
    /** An explicit counter reset, or null when numbering continues. */
    public val start: Long?,
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
