package com.nouprax.markdown.core

import java.nio.file.Files
import java.nio.file.Path

internal actual fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    DesktopNativeLoader.ensureLoaded()
    return JvmNative.parse(source, options.toNativeMask())
}

internal actual class NativeSession actual constructor(
    options: ParseOptions,
) {
    private val handle: Long

    init {
        DesktopNativeLoader.ensureLoaded()
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

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        val os = System.getProperty("os.name").lowercase()
        val architecture = System.getProperty("os.arch").lowercase()
        val platform =
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                os.contains("windows") && architecture in setOf("x86_64", "amd64") -> "windows-x64"
                else -> throw UnsupportedOperationException("unsupported native platform: $os/$architecture")
            }
        val filename = System.mapLibraryName("markdown_core_kotlin")
        val resource = "/com/nouprax/markdown/core/native/$platform/$filename"
        val directory = Files.createTempDirectory("markdown-core-")
        val library = directory.resolve(filename)

        // deleteOnExit removes entries in reverse registration order, so the
        // directory must be registered before its child.
        directory.toFile().deleteOnExit()
        requireNotNull(DesktopNativeLoader::class.java.getResourceAsStream(resource)) {
            "native library is missing for $platform"
        }.use { Files.copy(it, library) }
        library.toFile().deleteOnExit()
        loadBundledLibrary(library)
    }

    @Suppress("UnsafeDynamicallyLoadedCode")
    private fun loadBundledLibrary(library: Path) {
        // loadLibrary cannot address a native library extracted from this JAR.
        System.load(library.toAbsolutePath().toString())
    }
}
