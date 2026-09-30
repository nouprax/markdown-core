package com.nouprax.markdown.core

import java.nio.file.Files
import java.nio.file.Path

internal actual fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document {
    DesktopNativeLoader.ensureLoaded()
    return WireDecoder.decode(
        JniParser.parsePayload(source) ?: throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED),
        unit,
    )
}

private object JniParser {
    /**
     * The MCB3 message for [source], or null when the engine could not
     * allocate it or it exceeds a byte array's capacity.
     *
     * Kotlin `internal` is public bytecode on the JVM, so @JvmSynthetic hides
     * this raw method from Java source while JNI registration still finds it.
     */
    @JvmSynthetic
    external fun parsePayload(source: ByteArray): ByteArray?
}

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        // The package bundles one library per operating system it supports.
        val platform = if (System.getProperty("os.name").lowercase().contains("mac")) "macos-arm64" else "linux-x64"
        val filename = System.mapLibraryName("markdown_core_kotlin")
        val resource = "/com/nouprax/markdown/core/native/$platform/$filename"
        val directory = Files.createTempDirectory("markdown-core-")
        val library = directory.resolve(filename)

        // deleteOnExit removes entries in reverse registration order, so the
        // directory must be registered before its child.
        directory.toFile().deleteOnExit()
        DesktopNativeLoader::class.java.getResourceAsStream(resource).use { Files.copy(it, library) }
        library.toFile().deleteOnExit()
        loadBundledLibrary(library)
    }

    @Suppress("UnsafeDynamicallyLoadedCode")
    private fun loadBundledLibrary(library: Path) {
        // loadLibrary cannot address a native library extracted from this JAR.
        System.load(library.toAbsolutePath().toString())
    }
}
