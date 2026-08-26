package com.nouprax.markdown.core

internal actual fun nativeSessionNew(options: ParseOptions): Long {
    AndroidNativeLoader.ensureLoaded()
    val session = JvmNative.sessionNew(options.toNativeMask())
    if (session == 0L) throw OutOfMemoryError("native session allocation failed")
    return session
}

internal actual fun nativeSessionFeed(
    session: Long,
    chunk: ByteArray,
): ByteArray = JvmNative.sessionFeed(session, chunk)

internal actual fun nativeSessionFinish(session: Long): ByteArray = JvmNative.sessionFinish(session)

internal actual fun nativeSessionFree(session: Long) = JvmNative.sessionFree(session)

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
    external fun sessionNew(optionsMask: Int): Long

    external fun sessionFeed(
        session: Long,
        chunk: ByteArray,
    ): ByteArray

    external fun sessionFinish(session: Long): ByteArray

    external fun sessionFree(session: Long)
}
