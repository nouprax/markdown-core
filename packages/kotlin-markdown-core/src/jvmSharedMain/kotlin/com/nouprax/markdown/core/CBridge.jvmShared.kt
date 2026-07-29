@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import java.io.InputStream
import java.nio.file.Files
import java.nio.file.Path
import kotlin.jvm.JvmSynthetic

/** Loads the JNI bridge for this target before any [JvmNative] call. */
internal expect fun ensureNativeLoaded()

@JvmSynthetic
internal actual fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    ensureNativeLoaded()
    return JvmNative.parse(source, options.toNativeMask())
}

@JvmSynthetic
internal actual fun openCSession(options: ParseOptions): CSessionHandle {
    ensureNativeLoaded()
    val handle = JvmNative.sessionOpen(options.toNativeMask())
    if (handle == 0L) {
        throw OutOfMemoryError("native session allocation failed")
    }
    return handle
}

@JvmSynthetic
internal actual fun CSessionHandle.free(): Unit = JvmNative.sessionFree(this)

@JvmSynthetic
internal actual fun CSessionHandle.lineage(): ULong = JvmNative.sessionLineage(this).toULong()

@JvmSynthetic
internal actual fun CSessionHandle.revision(): ULong = JvmNative.sessionRevision(this).toULong()

@JvmSynthetic
internal actual fun CSessionHandle.length(): Long = JvmNative.sessionLength(this)

@JvmSynthetic
internal actual fun CSessionHandle.rootId(): ULong = JvmNative.sessionRoot(this).toULong()

@JvmSynthetic
internal actual fun CSessionHandle.edit(
    byteStart: Long,
    byteEnd: Long,
    replacement: ByteArray,
): ByteArray = JvmNative.sessionEdit(this, byteStart, byteEnd, replacement)

@JvmSynthetic
internal actual fun CSessionHandle.commit(): ByteArray = JvmNative.sessionCommit(this)

@JvmSynthetic
internal actual fun CSessionHandle.scopes(): ByteArray = JvmNative.sessionScopes(this)

@JvmSynthetic
internal actual fun CSessionHandle.footnoteInfo(id: ULong): ByteArray = JvmNative.sessionFootnoteInfo(this, id.toLong())

@JvmSynthetic
internal actual fun CSessionHandle.footnotes(): ByteArray = JvmNative.sessionFootnotes(this)

@JvmSynthetic
internal actual fun CSessionHandle.footnoteReferences(definition: ULong): ByteArray =
    JvmNative.sessionFootnoteReferences(this, definition.toLong())

private object JvmNative {
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
internal val hostNativeLibraryFileName: String
    @JvmSynthetic
    get() = System.mapLibraryName("markdown_core_kotlin")

/** The lowercase `os/arch` pair, for unsupported-host failures. */
internal val hostNativeLibraryHostName: String
    @JvmSynthetic
    get() =
        "${System.getProperty("os.name").orEmpty().lowercase()}/" +
            System.getProperty("os.arch").orEmpty().lowercase()

/** The resource-layout platform of this host, or null when the host is
 * outside the supported desktop matrix. */
internal val hostNativeLibraryPlatform: String?
    @JvmSynthetic
    get() =
        run {
            val os = System.getProperty("os.name").orEmpty().lowercase()
            val architecture = System.getProperty("os.arch").orEmpty().lowercase()
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                else -> null
            }
        }

/** The classpath location of this host's bundled library, or null on an
 * unsupported host. */
internal val hostNativeLibraryResourcePath: String?
    @JvmSynthetic
    get() =
        hostNativeLibraryPlatform?.let {
            "/com/nouprax/markdown/core/native/$it/$hostNativeLibraryFileName"
        }

/** Extracts a bundled library to a self-deleting temp directory and
 * loads it; [stream] is consumed and closed. */
@JvmSynthetic
internal fun extractAndLoadHostNativeLibrary(stream: InputStream) {
    val directory = Files.createTempDirectory("markdown-core-")
    val library = directory.resolve(hostNativeLibraryFileName)

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
