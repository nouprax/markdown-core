package com.nouprax.markdown.core

public data class Position(
    public val line: Int,
    public val column: Int,
)

public data class Scope(
    public val start: Position,
    public val end: Position,
)

public enum class ListFlavor { BULLET, ORDERED }
public enum class OrderedListStyle {
    DECIMAL,
    LOWER_ALPHA,
    UPPER_ALPHA,
    LOWER_ROMAN,
    UPPER_ROMAN,
    EXAMPLE,
    DEFAULT,
}

public enum class OrderedListDelimiter {
    PERIOD,
    ONE_PAREN,
    TWO_PARENS,
    DEFAULT,
}

public enum class PlacementMode { EMBEDDED, STANDALONE }

public enum class TableAlignment { NONE, LEFT, CENTER, RIGHT }

/** How a [CitationReferent.Bib] item is rendered. */
public enum class BibMode { NORMAL, AUTHOR_IN_TEXT, SUPPRESS_AUTHOR }
