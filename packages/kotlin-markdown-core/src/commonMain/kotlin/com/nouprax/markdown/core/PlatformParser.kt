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

    /**
     * Replaces the text from the cursor at line [startLine] and [startColumn] to
     * the cursor at line [endLine] and [endColumn] with the UTF-8 [text].
     */
    fun edit(
        startLine: Int,
        startColumn: Int,
        endLine: Int,
        endColumn: Int,
        text: ByteArray,
    ): Document

    /** The session's text in UTF-8. */
    fun text(): ByteArray

    /** Releases the engine's session once; a second call does nothing. */
    fun free()

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

/** A session of the UTF-8 [source], with columns counted in [unit]. */
internal expect fun newPlatformSession(
    source: ByteArray,
    unit: TextUnit,
): PlatformSession
