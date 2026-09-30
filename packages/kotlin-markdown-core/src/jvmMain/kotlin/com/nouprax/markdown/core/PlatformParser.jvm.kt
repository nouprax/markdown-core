package com.nouprax.markdown.core

import java.nio.file.Files
import java.nio.file.Path

internal actual fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document {
    DesktopNativeLoader.ensureLoaded()
    return decode(JniParser.parsePayload(source), unit)
}

/** The document of [message], whose absence is [ErrorCode.ALLOCATION_FAILED]. */
private fun decode(
    message: ByteArray?,
    unit: TextUnit,
): Document = WireDecoder.decode(message ?: throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED), unit)

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
        DesktopNativeLoader.ensureLoaded()
        document = decode(JniParser.sessionNewPayload(source, unit == TextUnit.UTF16, session), unit)
    }

    override fun edit(
        fields: IntArray,
        texts: ByteArray,
    ): Document = publish(JniParser.sessionEditPayload(session[0], fields, texts))

    override fun append(text: ByteArray): Document = publish(JniParser.sessionAppendPayload(session[0], text))

    override fun text(): ByteArray = JniParser.sessionText(session[0])

    override fun free() {
        JniParser.sessionFree(session[0])
        session[0] = 0
    }

    private fun publish(message: ByteArray?): Document = decode(message, document.unit).also { document = it }
}

private object JniParser {
    /**
     * The MCB3 message for [source], or null when the engine could not
     * allocate it or it exceeds a byte array's capacity.
     *
     * Kotlin `internal` is public bytecode on the JVM, so @JvmSynthetic hides
     * this raw method from Java source while JNI registration still finds it.
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

    /** The MCB3 message of an edit batch of [session] (see [PlatformSession.edit]); null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionEditPayload(
        session: Long,
        fields: IntArray,
        texts: ByteArray,
    ): ByteArray?

    /** The MCB3 message of appending [text] to [session]; null as for [parsePayload]. */
    @JvmSynthetic
    external fun sessionAppendPayload(
        session: Long,
        text: ByteArray,
    ): ByteArray?

    /** A copy of [session]'s UTF-8 text. */
    @JvmSynthetic
    external fun sessionText(session: Long): ByteArray

    /** Releases [session]; 0 releases nothing. */
    @JvmSynthetic
    external fun sessionFree(session: Long)
}

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        // The package bundles one library per operating system it supports.
        val platform = if (System.getProperty("os.name").lowercase().contains("mac")) "macos-arm64" else "linux-x64"
        val filename = System.mapLibraryName("markdown_core_kotlin")
        val resource = "/com/nouprax/markdown/core/native/$platform/$filename"
        val directory = Files.createTempDirectory("markdown-core-")
        val library = directory.resolve(filename)

        // deleteOnExit removes entries in reverse registration order, so the
        // directory must be registered before its child.
        directory.toFile().deleteOnExit()
        DesktopNativeLoader::class.java.getResourceAsStream(resource).use { Files.copy(it, library) }
        library.toFile().deleteOnExit()
        loadBundledLibrary(library)
    }

    @Suppress("UnsafeDynamicallyLoadedCode")
    private fun loadBundledLibrary(library: Path) {
        // loadLibrary cannot address a native library extracted from this JAR.
        System.load(library.toAbsolutePath().toString())
    }
}
