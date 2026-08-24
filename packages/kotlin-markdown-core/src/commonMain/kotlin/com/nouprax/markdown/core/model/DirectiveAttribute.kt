package com.nouprax.markdown.core

/**
 * One directive attribute. The sequence is sorted by name, so a pair is all
 * there is to say about one entry.
 */
public class DirectiveAttribute internal constructor(
    public val name: String,
    public val value: String,
)
