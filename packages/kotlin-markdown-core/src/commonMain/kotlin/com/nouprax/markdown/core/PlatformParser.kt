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

    /**
     * Applies a batch of `fields.size / 3` edits: each is three fields of
     * [fields] -- start, end, and the byte size of its text -- and their
     * texts follow one another in [texts] in the order listed.
     */
    fun edit(
        fields: IntArray,
        texts: ByteArray,
    ): Document

    /** Appends the UTF-8 [text]. */
    fun append(text: ByteArray): Document

    /** The session's text in UTF-8. */
    fun text(): ByteArray

    /** Releases the engine's session once; a second call does nothing. */
    fun free()
}

/** A session of the UTF-8 [source], with offsets and columns counted in [unit]. */
internal expect fun newPlatformSession(
    source: ByteArray,
    unit: TextUnit,
): PlatformSession
