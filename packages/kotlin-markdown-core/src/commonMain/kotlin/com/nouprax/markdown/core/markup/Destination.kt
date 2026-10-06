package com.nouprax.markdown.core

/**
 * The destination of a [Link], [Embedded], [Reference], [CrossLink], or [CrossEmbedded]: a tagged value, not a
 * node, so it has no scope and no children, and a branch's fields exist only in that branch.
 */
public sealed interface Destination {
    /**
     * The complete semantic destination the inherited grammar produced: the
     * bytes between angle brackets or the bare destination, with backslash
     * escapes and character references decoded and no percent-encoding,
     * normalization, or resolution. `[a]()` and `[a](<>)` wrote one and wrote
     * nothing in it, so [value] is empty. A direct link or image and every
     * [com.nouprax.markdown.core.Reference] own this branch.
     */
    @ConsistentCopyVisibility
    public data class Url internal constructor(
        public val value: String,
    ) : Destination

    /**
     * The workspace address a cross link produces: a [path] that may be empty
     * when the [anchor] addresses the current document, and the anchor or
     * `null`.
     */
    @ConsistentCopyVisibility
    public data class Cross internal constructor(
        public val path: String,
        public val anchor: String?,
    ) : Destination

    /**
     * A reference occurrence -- `[t][l]`, `[l][]` or `[l]` -- naming the
     * definition it resolves to by its normalized [label].
     * [Document.reference] answers the node the label resolves to.
     */
    @ConsistentCopyVisibility
    public data class Reference internal constructor(
        public val label: String,
    ) : Destination
}
