package com.nouprax.markdown.core

/**
 * A link reference definition where it was written: a leaf block with its
 * normalized label, the destination it states and its title. Its anchor and
 * attributes are the ones the definition states. Duplicates and definitions
 * nobody names remain. [Document.references] lists every reference in source
 * order, and [Document.reference] finds the node a label resolves to.
 */
public class Reference internal constructor(
    /**
     * The normalized label: case folded, trimmed, internal whitespace
     * collapsed. Exactly the [Destination.Reference.label] of every
     * occurrence that names this definition.
     */
    public val label: String,
    /** Always a [Destination.Url]: the destination the definition states. */
    public val dest: Destination,
    /** Optional: `[a]: /u` states no title, `[a]: /u ""` states an empty one. */
    public val title: String?,
    override val id: MarkupID,
    override val extent: Extent,
    override val pieces: kotlin.collections.List<Piece>,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
