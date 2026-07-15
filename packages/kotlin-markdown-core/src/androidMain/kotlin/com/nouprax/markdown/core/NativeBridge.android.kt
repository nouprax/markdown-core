package com.nouprax.markdown.core

internal actual fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    AndroidNativeLoader.ensureLoaded()
    return JvmNative.parse(source, options.toNativeMask())
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

internal object JvmNative {
    external fun parse(
        source: ByteArray,
        optionsMask: Int,
    ): ByteArray
}
