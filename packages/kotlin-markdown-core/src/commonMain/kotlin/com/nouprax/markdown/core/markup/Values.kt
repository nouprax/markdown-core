package com.nouprax.markdown.core

/** Native cmark UTF-8 editor coordinates, copied without conversion to Kotlin string indices.
 * A zero-byte document ends at (0, 0); other native column-zero sentinels are retained.
 */
public data class Position(
    public val line: Int,
    public val column: Int,
)

/**
 * Authored editor start/end coordinates, not a substring or half-open range.
 *
 * The four coordinates are the fields, so every node carries one object for
 * its scope rather than three; [start] and [end] are the same coordinates as
 * positions, made on request.
 */
public data class Scope(
    public val startLine: Int,
    public val startColumn: Int,
    public val endLine: Int,
    public val endColumn: Int,
) {
    public constructor(start: Position, end: Position) : this(start.line, start.column, end.line, end.column)

    public val start: Position get() = Position(startLine, startColumn)
    public val end: Position get() = Position(endLine, endColumn)
}

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

public enum class Placement { EMBEDDED, STANDALONE }

/** How a [CitationReferent.Bib] item is rendered. */
public enum class BibMode { NORMAL, AUTHOR_IN_TEXT, SUPPRESS_AUTHOR }
