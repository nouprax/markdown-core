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

public enum class PlacementMode { EMBEDDED, STANDALONE }

public enum class TableAlignment { NONE, LEFT, CENTER, RIGHT }
