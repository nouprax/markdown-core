package com.nouprax.markdown.core

/**
 * The target of a [Link], [Image], or [CrossLink]: a tagged value, not a node, so it has no
 * scope and no children, and a branch's fields exist only in that branch.
 */
public sealed interface Destination {
    /**
     * The complete semantic destination the inherited grammar produced: the
     * bytes between angle brackets or the bare destination, with backslash
     * escapes and character references decoded and no percent-encoding,
     * normalization, or resolution. `[a]()` and `[a](<>)` wrote one and wrote
     * nothing in it, so [value] is empty. Every link and image owns this
     * branch.
     */
    public class Url internal constructor(
        public val value: String,
    ) : Destination

    /**
     * The workspace address a cross link produces: a [path] that may be empty
     * when the [anchor] addresses the current document, and the anchor or
     * `null`.
     */
    public class Cross internal constructor(
        public val path: String,
        public val anchor: String?,
    ) : Destination
}
