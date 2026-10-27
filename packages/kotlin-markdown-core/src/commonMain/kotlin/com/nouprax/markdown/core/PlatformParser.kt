package com.nouprax.markdown.core

internal expect fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document

/**
 * The engine's session behind a [MarkdownSession]. Each step decodes the one
 * MCB3 message the engine answers with into the document it published, or
 * throws the failure that message carries, as [parsePlatformDocument] does.
 */
internal interface PlatformSession {
    /** The document the latest step published. */
    val document: Document

    /** Appends the UTF-8 [text]. */
    fun append(text: ByteArray): Document

    /** Ends the text: the final document. */
    fun end(): Document

    /** The session's text in UTF-8. */
    fun text(): ByteArray

    /** Releases the engine's session once; a second call does nothing. */
    fun free()

    /**
     * Moves the engine session's cursor to [line] and [column]: the MCB3
     * answer of the context the locate leaves as the session's state.
     */
    fun locate(
        line: Int,
        column: Int,
    ): ByteArray

    /** The MCB3 answer of spelling the UTF-8 [value] in slot [slot] of the session's context. */
    fun spell(
        slot: Int,
        value: ByteArray,
    ): ByteArray

    /**
     * The MCB3 answer of the scopes of the fields of slot kind [slot], 0 for
     * none, of the node with identifier [id] that holds the byte at [line]
     * and [column].
     */
    fun field(
        id: Long,
        line: Int,
        column: Int,
        slot: Int,
    ): ByteArray

    /** The MCB3 answer of the scopes of the markers of the node [field] names. */
    fun markers(
        id: Long,
        line: Int,
        column: Int,
    ): ByteArray
}

/** The MCB3 answer of the names and the characters the dialect fixes. */
internal expect fun platformDialect(): ByteArray

/** A session of the UTF-8 [source], with columns counted in [unit]. */
internal expect fun newPlatformSession(
    source: ByteArray,
    unit: TextUnit,
): PlatformSession
