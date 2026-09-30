package com.nouprax.markdown.core

internal actual fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document {
    AndroidNativeLoader.ensureLoaded()
    return WireDecoder.decode(JniParser.parsePayload(source), unit)
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

private object JniParser {
    @JvmSynthetic
    external fun parsePayload(source: ByteArray): ByteArray
}
