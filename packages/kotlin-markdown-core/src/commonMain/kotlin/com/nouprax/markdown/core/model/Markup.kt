package com.nouprax.markdown.core

public sealed interface Markup {
    /**
     * The node's identity (docs/STREAMING.md §4 D4): the name a consumer
     * tracks this element by across a stream's feeds -- the render key. A
     * block keeps its identity across feeds however the bytes arrive; an
     * inline's is stable exactly as long as its owning block's inline list is.
     */
    public val id: Identity

    public val scope: Scope

    public fun <Result> accept(visitor: Visitor<Result>): Result

    /** Returns the canonical diagnostic dump for this markup subtree. */
    public fun dump(): String = TreeDumper.dump(this)
}
