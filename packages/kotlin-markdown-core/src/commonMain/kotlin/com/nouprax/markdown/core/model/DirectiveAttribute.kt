package com.nouprax.markdown.core

/**
 * One directive attribute. Its sequence preserves first-occurrence source
 * order; later occurrences update or accumulate in that original position.
 */
public class DirectiveAttribute internal constructor(
    public val name: String,
    public val value: String,
)
