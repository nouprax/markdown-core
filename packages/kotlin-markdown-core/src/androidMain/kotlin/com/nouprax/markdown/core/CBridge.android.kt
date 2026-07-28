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
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        if (System.getProperty("java.vm.name").orEmpty().contains("Dalvik", ignoreCase = true)) {
            System.loadLibrary("markdown_core_kotlin")
            return
        }
        // Host-JVM execution (Robolectric and other local unit tests). Three
        // supported layers: an explicit library path, a host build of the
        // library on the classpath at the desktop resource layout, and an
        // actionable failure naming both remedies.
        val explicit = System.getProperty("markdown.core.hostNativeLibrary")
        if (explicit != null) {
            System.load(explicit)
            return
        }
        val os = System.getProperty("os.name").lowercase()
        val architecture = System.getProperty("os.arch").lowercase()
        val platform =
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                os.contains("windows") && architecture in setOf("x86_64", "amd64") -> "windows-x64"
                else -> null
            }
        val filename = System.mapLibraryName("markdown_core_kotlin")
        val resource = platform?.let { "/com/nouprax/markdown/core/native/$it/$filename" }
        val stream = resource?.let { AndroidNativeLoader::class.java.getResourceAsStream(it) }
        if (stream == null) {
            throw IllegalStateException(
                "Markdown Core's Android artifact is running on a host JVM without its native " +
                    "library. Either set -Dmarkdown.core.hostNativeLibrary=/path/to/$filename " +
                    "to a host build of markdown_core_kotlin, or put a host build on the test " +
                    "classpath at ${resource ?: "com/nouprax/markdown/core/native/<platform>/$filename"}.",
            )
        }
        val directory =
            java.nio.file.Files
                .createTempDirectory("markdown-core-")
        val library = directory.resolve(filename)
        // deleteOnExit removes entries in reverse registration order, so the
        // directory must be registered before its child.
        directory.toFile().deleteOnExit()
        stream.use {
            java.nio.file.Files
                .copy(it, library)
        }
        library.toFile().deleteOnExit()
        System.load(library.toAbsolutePath().toString())
    }
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
