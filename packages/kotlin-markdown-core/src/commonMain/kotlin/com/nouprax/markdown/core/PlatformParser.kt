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

/**
 * The language engine's server behind [com.nouprax.markdown.core.lsp.Server]:
 * each call is one call of the C engine `markdown-core-lsp`, and answers its
 * status, 0 or the protocol's error code.
 */
internal interface PlatformServer {
    /** The engine's phase: 0 fresh, 1 active, 2 halted. */
    fun phase(): Int

    /**
     * Initializes with the position encodings [units], each true for UTF-16
     * and false for UTF-8. On success `answer[1]` is 1 when the server counts
     * in UTF-16 and 0 in UTF-8, and the result is the cues' bytes; `answer[0]`
     * is the status.
     */
    fun initialize(
        units: BooleanArray,
        answer: IntArray,
    ): ByteArray

    fun shutdown(): Int

    fun open(
        uri: ByteArray,
        version: Int,
        text: ByteArray,
    ): Int

    /**
     * Applies the edits whose scopes [scopes] holds, four numbers each (start
     * line and character, end line and character, read as unsigned), and
     * whose texts are [text] cut at [ends].
     */
    fun update(
        uri: ByteArray,
        version: Int,
        scopes: IntArray,
        text: ByteArray,
        ends: IntArray,
    ): Int

    fun close(uri: ByteArray): Int

    /** Completes at [line] and [character]; on success `answer` holds the list's version and count. */
    fun complete(
        uri: ByteArray,
        line: Int,
        character: Int,
        answer: LongArray,
    ): Int

    /** Releases the engine's server once; a second call does nothing. */
    fun free()
}

/** A new engine server, or null when it cannot be allocated. */
internal expect fun newPlatformServer(): PlatformServer?
