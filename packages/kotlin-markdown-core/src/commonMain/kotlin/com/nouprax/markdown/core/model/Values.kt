package com.nouprax.markdown.core

public data class Position(
    public val line: Int,
    public val column: Int,
)

public data class Scope(
    public val start: Position,
    public val end: Position,
)

/**
 * A node's identity: the name a consumer tracks an element by across a
 * stream's feeds. [block] is the owning block's document-unique mint --
 * the block is the minimal update unit, so it alone names the region an
 * incremental consumer re-renders -- and [ordinal] is the node's pre-order
 * ordinal among that block's inline descendants, 0 for the block itself.
 * The pair is unique within one document and never reused within a parse;
 * it is not stable across documents. The halves are opaque values: compare
 * them, key maps by them, and derive nothing else from them.
 */
public data class Identity(
    public val block: Int,
    public val ordinal: Int,
)

public enum class ListFlavor { BULLET, ORDERED }

public enum class PlacementMode { EMBEDDED, STANDALONE }

public enum class TableAlignment { NONE, LEFT, CENTER, RIGHT }
