package com.nouprax.markdown.core

internal actual fun parsePlatformDocument(
    source: ByteArray,
    options: ParseOptions,
): Document {
    AndroidNativeLoader.ensureLoaded()
    return JniPayloadDecoder.decodeDocument(JniParser.parsePayload(source, options.toNativeMask()))
}

private object AndroidNativeLoader {
    private val loaded: Unit =
        if (System.getProperty("java.vm.name").orEmpty().contains("Dalvik", ignoreCase = true)) {
            System.loadLibrary("markdown_core_kotlin")
        } else {
            System.load(requireNotNull(System.getProperty("markdown.core.hostNativeLibrary")))
        }

    fun ensureLoaded() = loaded
}

private object JniParser {
    @JvmSynthetic
    external fun parsePayload(
        source: ByteArray,
        optionsMask: Int,
    ): ByteArray
}
