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

internal object JvmNative {
    external fun parse(
        source: ByteArray,
        optionsMask: Int,
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
