@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import java.io.InputStream
import java.lang.ref.Cleaner
import java.nio.file.Files
import java.nio.file.Path
import kotlin.jvm.JvmSynthetic

/** Loads the JNI bridge for this target before any [JvmNative] call. */
@JvmSynthetic
internal expect fun ensureNativeLoaded()

@JvmSynthetic
internal actual fun cOpen(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    ensureNativeLoaded()
    return JvmNative.open(source, options.toNativeMask())
}

@JvmSynthetic
internal actual fun CDocumentHandle.edit(source: ByteArray): ByteArray = JvmNative.edit(this, source)

@JvmSynthetic
internal actual fun CDocumentHandle.release(): Unit = JvmNative.release(this)

/** One Cleaner for the process: its thread is started on first use and is
 * shared by every document, which is what the class is for. */
private val cleaner: Cleaner by lazy { Cleaner.create() }

@JvmSynthetic
internal actual fun attachNativeCleanup(
    owner: Any,
    release: () -> Unit,
): Any? =
    // The Runnable holds `release` and never `owner`; one that reached its
    // owner would keep that owner reachable and never run. The Cleanable is
    // handed back only because the shared signature has to serve
    // Kotlin/Native; the JVM's registration is live whether anyone holds it
    // or not.
    cleaner.register(owner) { release() }

private object JvmNative {
    external fun open(
        source: ByteArray,
        optionsMask: Int,
    ): ByteArray

    external fun edit(
        handle: Long,
        source: ByteArray,
    ): ByteArray

    external fun release(handle: Long)
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
