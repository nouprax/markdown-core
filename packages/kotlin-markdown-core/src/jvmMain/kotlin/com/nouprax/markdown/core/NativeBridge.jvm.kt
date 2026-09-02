package com.nouprax.markdown.core

import java.nio.file.Files
import java.nio.file.Path

internal actual fun nativeSessionNew(options: ParseOptions): Long {
    DesktopNativeLoader.ensureLoaded()
    val session = JvmNative.sessionNew(options.toNativeMask())
    if (session == 0L) throw OutOfMemoryError("native session allocation failed")
    return session
}

internal actual fun nativeSessionFeed(
    session: Long,
    chunk: ByteArray,
    request: Int,
): ByteArray = JvmNative.sessionFeed(session, chunk, request)

internal actual fun nativeSessionAdvance(
    session: Long,
    chunk: ByteArray,
): ByteArray = JvmNative.sessionAdvance(session, chunk)

internal actual fun nativeSessionFinish(
    session: Long,
    request: Int,
): ByteArray = JvmNative.sessionFinish(session, request)

internal actual fun nativeSessionFree(session: Long) = JvmNative.sessionFree(session)

internal object JvmNative {
    // `@JvmSynthetic` because `internal` IS NOT PRIVATE ON THE JVM. This object
    // is reached from another compilation unit, so Kotlin has to emit it
    // `public final`, and without this a Java caller can invoke the JNI entry
    // point directly -- handing it a byte array and an options mask the decoder
    // never sees. JNI resolves by name and descriptor and does not consult the
    // synthetic flag, so the binding still links; only javac stops resolving it.
    // The session entries carry it for the same reason, plus one of their own:
    // a raw `long` handle freed twice or fed after free is native memory
    // corruption, and only `Document` sequences those calls.
    @JvmSynthetic
    external fun sessionNew(optionsMask: Int): Long

    @JvmSynthetic
    external fun sessionFeed(
        session: Long,
        chunk: ByteArray,
        request: Int,
    ): ByteArray

    @JvmSynthetic
    external fun sessionAdvance(
        session: Long,
        chunk: ByteArray,
    ): ByteArray

    @JvmSynthetic
    external fun sessionFinish(
        session: Long,
        request: Int,
    ): ByteArray

    @JvmSynthetic
    external fun sessionFree(session: Long)
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
