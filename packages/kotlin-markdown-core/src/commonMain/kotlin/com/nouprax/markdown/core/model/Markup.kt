package com.nouprax.markdown.core

public sealed interface Markup {
    public val scope: Scope

    public fun <Result> accept(visitor: Visitor<Result>): Result

    /** Returns the canonical debug dump for this markup subtree. */
    public fun dump(): String = TreeDumper.dump(this)
}
