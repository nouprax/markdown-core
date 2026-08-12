package com.nouprax.markdown.core

/** A one-based line/column source coordinate. */
public data class Position(
    public val line: Int,
    public val column: Int,
)

/**
 * An absolute source extent: two coordinates, both inclusive of whatever
 * markers the construct wrote around itself.
 */
public data class Scope(
    public val start: Position,
    public val end: Position,
)

/** Whether a list's items are bulleted or numbered.
 *
 * [List.start] carries a number only for [ORDERED]. */
public enum class ListFlavor { BULLET, ORDERED }

/**
 * Whether a construct takes part in the inline flow around it or is presented
 * independently of that flow.
 *
 * Not a statement about where the node sits: a [Formula] can be [STANDALONE]
 * and still be inside a [Paragraph].
 */
public enum class PlacementMode { EMBEDDED, STANDALONE }

/** A table column's alignment, as the `:` markers of the delimiter row
 * declared it.
 *
 * [NONE] when that column's delimiter carried neither. */
public enum class TableAlignment { NONE, LEFT, CENTER, RIGHT }
