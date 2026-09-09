package com.nouprax.markdown.core

/** A node-independent size. Every present component is a positive 32-bit integer. */
public data class Dimensions(
    /** Required width in 1..2147483647. */
    public val width: Int,
    /** Height in 1..2147483647, or null when unspecified. */
    public val height: Int? = null,
) {
    init {
        require(width > 0) { "invalid dimension width" }
        require(height == null || height > 0) { "invalid dimension height" }
    }
}
