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

private object JniParser {
    /**
     * The MCB3 message for [source], or null when the engine could not
     * allocate it or it exceeds a byte array's capacity.
     */
    @JvmSynthetic
    external fun parsePayload(source: ByteArray): ByteArray?
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
