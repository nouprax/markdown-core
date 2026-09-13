package com.nouprax.markdown.core

/** Native cmark UTF-8 editor coordinates, copied without conversion to Kotlin string indices.
 * A zero-byte document ends at (0, 0); other native column-zero sentinels are retained.
 */
public data class Position(
    public val line: Int,
    public val column: Int,
)

/** Authored editor start/end coordinates, not a substring or half-open range. */
public data class Scope(
    public val start: Position,
    public val end: Position,
)

public enum class ListFlavor { BULLET, ORDERED }

public sealed interface OrderedListVariant {
    public data object Decimal : OrderedListVariant

    public class Alpha internal constructor(
        public val lowercased: Boolean,
    ) : OrderedListVariant

    public class Roman internal constructor(
        public val lowercased: Boolean,
    ) : OrderedListVariant

    public data object Default : OrderedListVariant
}

public sealed interface OrderedListDelimiter {
    public data object Period : OrderedListDelimiter

    public class Parenthesis internal constructor(
        public val closed: Boolean,
    ) : OrderedListDelimiter

    public data object Default : OrderedListDelimiter
}

public enum class PlacementMode { EMBEDDED, STANDALONE }

public enum class TableAlignment { NONE, LEFT, CENTER, RIGHT }

/** How a [CitationReferent.Bib] item is rendered. */
public enum class BibMode { NORMAL, AUTHOR_IN_TEXT, SUPPRESS_AUTHOR }
