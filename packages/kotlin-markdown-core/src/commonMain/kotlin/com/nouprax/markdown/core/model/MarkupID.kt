package com.nouprax.markdown.core

/**
 * Session-scoped node identity: [rawValue] is unique within the owning
 * session and never reused; [lineage] is the session's random salt, so nodes
 * from different sessions (including separate one-shot parses) never share
 * an identity. Stable across incremental commits while the node remains the
 * same kind of thing at the same place.
 */
public data class MarkupID(
    public val lineage: ULong,
    public val rawValue: ULong,
)
