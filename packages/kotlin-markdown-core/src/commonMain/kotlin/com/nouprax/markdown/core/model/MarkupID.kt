package com.nouprax.markdown.core

import kotlin.jvm.JvmStatic

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
) {
    /** [lineage] as a bit-preserving signed value: the Java view of the
     * unsigned accessor, whose mangled name Java sources cannot write. */
    public fun lineageBits(): Long = lineage.toLong()

    /** [rawValue] as a bit-preserving signed value for Java callers. */
    public fun rawValueBits(): Long = rawValue.toLong()

    public companion object {
        /** Builds an identity from bit-preserving signed values — the Java
         * counterpart of the unsigned constructor. */
        @JvmStatic
        public fun fromBits(
            lineage: Long,
            rawValue: Long,
        ): MarkupID = MarkupID(lineage.toULong(), rawValue.toULong())
    }
}
