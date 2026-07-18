package com.nouprax.markdown.core

internal actual fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    AndroidNativeLoader.ensureLoaded()
    return JvmNative.parse(source, options.toNativeMask())
}

internal actual class CSession actual constructor(
    options: ParseOptions,
) {
    private val handle: Long

    init {
        AndroidNativeLoader.ensureLoaded()
        handle = JvmNative.sessionOpen(options.toNativeMask())
        if (handle == 0L) {
            throw OutOfMemoryError("native session allocation failed")
        }
    }

    actual fun free(): Unit = JvmNative.sessionFree(handle)

    actual fun lineage(): ULong = JvmNative.sessionLineage(handle).toULong()

    actual fun revision(): ULong = JvmNative.sessionRevision(handle).toULong()

    actual fun length(): Long = JvmNative.sessionLength(handle)

    actual fun rootId(): ULong = JvmNative.sessionRoot(handle).toULong()

    actual fun edit(
        byteStart: Long,
        byteEnd: Long,
        replacement: ByteArray,
    ): ByteArray = JvmNative.sessionEdit(handle, byteStart, byteEnd, replacement)

    actual fun commit(): ByteArray = JvmNative.sessionCommit(handle)

    actual fun scopes(): ByteArray = JvmNative.sessionScopes(handle)

    actual fun footnoteInfo(id: ULong): ByteArray = JvmNative.sessionFootnoteInfo(handle, id.toLong())

    actual fun footnotes(): ByteArray = JvmNative.sessionFootnotes(handle)

    actual fun footnoteReferences(definition: ULong): ByteArray =
        JvmNative.sessionFootnoteReferences(handle, definition.toLong())
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

    external fun sessionOpen(optionsMask: Int): Long

    external fun sessionFree(handle: Long)

    external fun sessionLineage(handle: Long): Long

    external fun sessionRevision(handle: Long): Long

    external fun sessionLength(handle: Long): Long

    external fun sessionRoot(handle: Long): Long

    external fun sessionEdit(
        handle: Long,
        byteStart: Long,
        byteEnd: Long,
        replacement: ByteArray,
    ): ByteArray

    external fun sessionCommit(handle: Long): ByteArray

    external fun sessionScopes(handle: Long): ByteArray

    external fun sessionFootnoteInfo(
        handle: Long,
        id: Long,
    ): ByteArray

    external fun sessionFootnotes(handle: Long): ByteArray

    external fun sessionFootnoteReferences(
        handle: Long,
        definition: Long,
    ): ByteArray
}
