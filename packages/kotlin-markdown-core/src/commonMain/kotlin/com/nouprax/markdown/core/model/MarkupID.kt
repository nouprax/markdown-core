package com.nouprax.markdown.core

import kotlin.jvm.JvmStatic

/**
 * Series-scoped node identity.
 *
 * [rawValue] is unique within the owning series and never reused; [series] is
 * that series' random salt, so nodes from different series (including
 * separate one-shot parses) never share an identity. Stable across appends
 * while the node remains the same kind of thing at the same place.
 *
 * A SERIES is one document and every document its appends produce. Raw values
 * restart at 1 for each new series, so the salt is the only thing keeping two
 * unrelated documents' identities apart.
 */
public data class MarkupID(
    public val series: ULong,
    public val rawValue: ULong,
) {
    /** [series] as a bit-preserving signed value: the Java view of the
     * unsigned accessor, whose mangled name Java sources cannot write. */
    public fun seriesBits(): Long = series.toLong()

    /** [rawValue] as a bit-preserving signed value for Java callers. */
    public fun rawValueBits(): Long = rawValue.toLong()

    public companion object {
        /** Builds an identity from bit-preserving signed values — the Java
         * counterpart of the unsigned constructor. */
        @JvmStatic
        public fun fromBits(
            series: Long,
            rawValue: Long,
        ): MarkupID = MarkupID(series.toULong(), rawValue.toULong())
    }
}
