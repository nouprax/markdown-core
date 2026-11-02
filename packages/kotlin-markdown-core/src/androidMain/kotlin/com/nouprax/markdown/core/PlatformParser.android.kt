package com.nouprax.markdown.core

internal actual fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document {
    AndroidNativeLoader.ensureLoaded()
    return decode(JniParser.parsePayload(source), unit)
}

/** The document of [message], whose absence is [ErrorCode.ALLOCATION_FAILED]. */
private fun decode(
    message: ByteArray?,
    unit: TextUnit,
): Document = WireDecoder.decode(message ?: throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED), unit)

internal actual fun platformDialect(): ByteArray {
    AndroidNativeLoader.ensureLoaded()
    return answer(JniParser.dialectAnswer())
}

/** An answer, whose absence is [ErrorCode.ALLOCATION_FAILED]. */
private fun answer(message: ByteArray?): ByteArray = message ?: throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED)

internal actual fun newPlatformSession(
    source: ByteArray,
    unit: TextUnit,
): PlatformSession = JniSession(source, unit)

/** A C session held through JNI by its address. */
private class JniSession(
    source: ByteArray,
    unit: TextUnit,
) : PlatformSession {
    /** The C session's address, which the engine writes when it makes the session; 0 once freed. */
    private val session = LongArray(1)

    override var document: Document
        private set

    init {
        AndroidNativeLoader.ensureLoaded()
        document = decode(JniParser.sessionNewPayload(source, unit == TextUnit.UTF16, session), unit)
    }

    override fun append(text: ByteArray): Document = publish(JniParser.sessionAppendPayload(session[0], text))

    override fun end(): Document = publish(JniParser.sessionFlushPayload(session[0]))

    override fun edit(
        startLine: Int,
        startColumn: Int,
        endLine: Int,
        endColumn: Int,
        text: ByteArray,
    ): Document = publish(JniParser.sessionEditPayload(session[0], startLine, startColumn, endLine, endColumn, text))

    override fun text(): ByteArray = JniParser.sessionText(session[0])

    override fun free() {
        JniParser.sessionClose(session[0])
        session[0] = 0
    }

    override fun locate(
        line: Int,
        column: Int,
    ): ByteArray = answer(JniParser.sessionLocateAnswer(session[0], line, column))

    override fun spell(
        slot: Int,
        value: ByteArray,
    ): ByteArray = answer(JniParser.sessionSpellAnswer(session[0], slot, value))

    override fun field(
        id: Long,
        line: Int,
        column: Int,
        slot: Int,
    ): ByteArray = answer(JniParser.sessionFieldAnswer(session[0], id, line, column, slot))

    override fun markers(
        id: Long,
        line: Int,
        column: Int,
    ): ByteArray = answer(JniParser.sessionMarkersAnswer(session[0], id, line, column))

    private fun publish(message: ByteArray?): Document = decode(message, document.unit).also { document = it }
}

internal actual fun newPlatformServer(): PlatformServer? {
    AndroidNativeLoader.ensureLoaded()
    val server = LongArray(1)
    return if (JniParser.serverNew(server) == 0) JniServer(server[0]) else null
}

/** A C language server held through JNI by its address. */
private class JniServer(
    /** The C server's address; 0 once freed. */
    private var server: Long,
) : PlatformServer {
    override fun phase(): Int = JniParser.serverPhase(server)

    override fun initialize(
        units: BooleanArray,
        answer: IntArray,
    ): ByteArray = JniParser.serverInitialize(server, units, answer) ?: ByteArray(0)

    override fun shutdown(): Int = JniParser.serverShutdown(server)

    override fun open(
        uri: ByteArray,
        version: Int,
        text: ByteArray,
    ): Int = JniParser.serverOpen(server, uri, version, text)

    override fun update(
        uri: ByteArray,
        version: Int,
        scopes: IntArray,
        text: ByteArray,
        ends: IntArray,
    ): Int = JniParser.serverUpdate(server, uri, version, scopes, text, ends)

    override fun close(uri: ByteArray): Int = JniParser.serverClose(server, uri)

    override fun complete(
        uri: ByteArray,
        line: Int,
        character: Int,
        answer: LongArray,
    ): Int = JniParser.serverComplete(server, uri, line, character, answer)

    override fun free() {
        JniParser.serverFree(server)
        server = 0
    }
}

private object JniParser {
    /**
     * The MCB3 message for [source], or null when the engine could not
     * allocate it or it exceeds a byte array's capacity.
     */
    @JvmSynthetic
    external fun parsePayload(source: ByteArray): ByteArray?

    /**
     * The MCB3 message of a new session of [source], counted in UTF-16 when
     * [utf16] and in UTF-8 otherwise, whose address the engine writes to
     * `session[0]`; null as for [parsePayload].
     */
    @JvmSynthetic
    external fun sessionNewPayload(
        source: ByteArray,
        utf16: Boolean,
        session: LongArray,
    ): ByteArray?

    /** The MCB3 message of appending [text] to [session]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionAppendPayload(
        session: Long,
        text: ByteArray,
    ): ByteArray?

    /** The MCB3 message of flushing [session]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionFlushPayload(session: Long): ByteArray?

    /**
     * The MCB3 message of replacing [session]'s text from the cursor at line
     * [startLine] and [startColumn] to the cursor at line [endLine] and [endColumn]
     * with [text]; null as for [parsePayload].
     */
    @JvmSynthetic
    external fun sessionEditPayload(
        session: Long,
        startLine: Int,
        startColumn: Int,
        endLine: Int,
        endColumn: Int,
        text: ByteArray,
    ): ByteArray?

    /** A copy of [session]'s UTF-8 text. */
    @JvmSynthetic
    external fun sessionText(session: Long): ByteArray

    /** Releases [session]; 0 releases nothing. */
    @JvmSynthetic
    external fun sessionClose(session: Long)

    /** The MCB3 answer of the dialect's names and characters; null as for [parsePayload]. */
    @JvmSynthetic
    external fun dialectAnswer(): ByteArray?

    /** The MCB3 answer of locating [session] at [line] and [column]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionLocateAnswer(
        session: Long,
        line: Int,
        column: Int,
    ): ByteArray?

    /** The MCB3 answer of spelling [value] in slot [slot] of [session]'s context; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionSpellAnswer(
        session: Long,
        slot: Int,
        value: ByteArray,
    ): ByteArray?

    /** The MCB3 answer of a field query of [session]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionFieldAnswer(
        session: Long,
        id: Long,
        line: Int,
        column: Int,
        slot: Int,
    ): ByteArray?

    /** The MCB3 answer of a marker query of [session]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionMarkersAnswer(
        session: Long,
        id: Long,
        line: Int,
        column: Int,
    ): ByteArray?

    /** Writes a new language server's address to `server[0]`: the engine's status. */
    @JvmSynthetic
    external fun serverNew(server: LongArray): Int

    /** Releases [server]; 0 releases nothing. */
    @JvmSynthetic
    external fun serverFree(server: Long)

    /** [server]'s phase. */
    @JvmSynthetic
    external fun serverPhase(server: Long): Int

    /** Initializes [server] as [PlatformServer.initialize] does. */
    @JvmSynthetic
    external fun serverInitialize(
        server: Long,
        units: BooleanArray,
        answer: IntArray,
    ): ByteArray?

    /** Shuts [server] down: the engine's status. */
    @JvmSynthetic
    external fun serverShutdown(server: Long): Int

    /** Opens [uri] in [server]: the engine's status. */
    @JvmSynthetic
    external fun serverOpen(
        server: Long,
        uri: ByteArray,
        version: Int,
        text: ByteArray,
    ): Int

    /** Edits [uri] in [server] as [PlatformServer.update] does: the engine's status. */
    @JvmSynthetic
    external fun serverUpdate(
        server: Long,
        uri: ByteArray,
        version: Int,
        scopes: IntArray,
        text: ByteArray,
        ends: IntArray,
    ): Int

    /** Closes [uri] in [server]: the engine's status. */
    @JvmSynthetic
    external fun serverClose(
        server: Long,
        uri: ByteArray,
    ): Int

    /** Completes in [uri] of [server] as [PlatformServer.complete] does: the engine's status. */
    @JvmSynthetic
    external fun serverComplete(
        server: Long,
        uri: ByteArray,
        line: Int,
        character: Int,
        answer: LongArray,
    ): Int
}

private object AndroidNativeLoader {
    private val loaded: Unit =
        if (System.getProperty("java.vm.name").contains("Dalvik", ignoreCase = true)) {
            System.loadLibrary("markdown_core_kotlin")
        } else {
            System.load(System.getProperty("markdown.core.hostNativeLibrary"))
        }

    fun ensureLoaded() = loaded
}
