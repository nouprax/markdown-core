package com.nouprax.markdown.core

/** A document-owned specimen definition, outside the markup union; syntax lands with P9b. */
public class Specimen internal constructor(
    /** The authored label, or null for an anonymous definition. */
    public val id: String?,
    /** An explicit counter reset, or null when numbering continues. */
    public val start: Long?,
    public val content: kotlin.collections.List<Markup>,
    public val scope: Scope,
)
