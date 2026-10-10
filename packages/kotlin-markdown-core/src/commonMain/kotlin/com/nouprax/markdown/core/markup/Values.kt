package com.nouprax.markdown.core

import kotlin.jvm.JvmInline

/**
 * A node's identifier: unique within its document across every owned
 * relation, and below 2^53. Identifiers of different documents are not
 * comparable.
 */
@JvmInline
public value class MarkupID(
    public val value: Long,
)

/**
 * The range a node covers, in bytes of the input of the parser that produced
 * it: a block's in the UTF-8 source, an inline node's in its inline root's
 * content, which starts at 0. [lead] is the signed distance from the end of
 * the previous node in the same relation, or from the owner's start for the
 * first node, to this node's start, and [span] is the length of its range.
 */
public data class Extent(
    public val lead: Int,
    public val span: UInt,
)

/**
 * A run of a node's own source: a source range whose [lead] is signed from
 * the end of the run before, or, for the first run, from the end of the source
 * of the previous node in the same relation (or from the start of the owner's
 * source, for a relation's first node), and whose [span] is its length in
 * bytes. A node's source starts where its first run starts and ends where its
 * last ends; between them its runs cover exactly its own source, so the source
 * between two runs is not the node's. Every node has at least one run.
 */
public data class Run(
    public val lead: Int,
    public val span: UInt,
)

/** How a document counts the columns of its scope queries: UTF-8 bytes or UTF-16 code units. */
public enum class TextUnit { UTF8, UTF16 }

/** Editor coordinates: a line counted from 1, and a column counted from 1 in the document's [TextUnit]. */
public data class Position(
    public val line: Int,
    public val column: Int,
)

/**
 * The editor coordinates of one of a node's source ranges, computed on
 * request by [Document.scope]: [start] is the position of the range's first
 * byte, and [end] the line holding the byte just past its last byte with the
 * column count from that line's start to it, so a range that ends right after
 * a line terminator ends at `L:0`, and a zero-byte document is `1:1..1:0`.
 */
public data class Scope(
    public val start: Position,
    public val end: Position,
)

public enum class ListFlavor { BULLET, ORDERED }

public sealed interface OrderedListVariant {
    public data object Decimal : OrderedListVariant

    @ConsistentCopyVisibility
    public data class Alpha internal constructor(
        public val lowercased: Boolean,
    ) : OrderedListVariant

    @ConsistentCopyVisibility
    public data class Roman internal constructor(
        public val lowercased: Boolean,
    ) : OrderedListVariant

    public data object Default : OrderedListVariant
}

public sealed interface OrderedListDelimiter {
    public data object Period : OrderedListDelimiter

    @ConsistentCopyVisibility
    public data class Parenthesis internal constructor(
        public val closed: Boolean,
    ) : OrderedListDelimiter

    public data object Default : OrderedListDelimiter
}

public enum class Placement { EMBEDDED, STANDALONE }

/** How a [CitationReferent.Bib] item is rendered. */
public enum class BibMode { NORMAL, AUTHOR_IN_TEXT, SUPPRESS_AUTHOR }
