package com.nouprax.markdown.core

import java.io.InputStream
import java.nio.file.Files
import java.nio.file.Path

/** Loads the JNI bridge for this target before any [JvmNative] call. */
internal expect fun ensureNativeLoaded()

internal actual fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    ensureNativeLoaded()
    return JvmNative.parse(source, options.toNativeMask())
}

internal actual class CSession actual constructor(
    options: ParseOptions,
) {
    private val handle: Long

    init {
        ensureNativeLoaded()
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

/**
 * The one description of a host build of the JNI bridge: its supported
 * platforms, its classpath resource layout, and the temp-directory
 * extraction both host loaders share.
 */
internal object HostNativeLibrary {
    val fileName: String = System.mapLibraryName("markdown_core_kotlin")

    /** The lowercase `os/arch` pair, for unsupported-host failures. */
    val hostName: String =
        "${System.getProperty("os.name").lowercase()}/${System.getProperty("os.arch").lowercase()}"

    /** The resource-layout platform of this host, or null when the host is
     * outside the supported desktop matrix. */
    val platform: String? =
        run {
            val os = System.getProperty("os.name").lowercase()
            val architecture = System.getProperty("os.arch").lowercase()
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                else -> null
            }
        }

    /** The classpath location of this host's bundled library, or null on an
     * unsupported host. */
    val resourcePath: String? = platform?.let { "/com/nouprax/markdown/core/native/$it/$fileName" }

    /** Extracts a bundled library to a self-deleting temp directory and
     * loads it; [stream] is consumed and closed. */
    fun extractAndLoad(stream: InputStream) {
        val directory = Files.createTempDirectory("markdown-core-")
        val library = directory.resolve(fileName)

        // deleteOnExit removes entries in reverse registration order, so the
        // directory must be registered before its child.
        directory.toFile().deleteOnExit()
        stream.use { Files.copy(it, library) }
        library.toFile().deleteOnExit()
        loadBundledLibrary(library)
    }

    @Suppress("UnsafeDynamicallyLoadedCode")
    private fun loadBundledLibrary(library: Path) {
        // loadLibrary cannot address a native library extracted from a JAR.
        System.load(library.toAbsolutePath().toString())
    }
}
